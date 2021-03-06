// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2004-2006 Sage Weil <sage@newdream.net>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#ifndef CEPH_OSD_SESSION_H
#define CEPH_OSD_SESSION_H

#include "common/RefCountedObj.h"
#include "common/Mutex.h"
#include "include/spinlock.h"
#include "OSDCap.h"
#include "Watch.h"
#include "OSDMap.h"

//#define PG_DEBUG_REFS

struct Session;
typedef boost::intrusive_ptr<Session> SessionRef;
struct Backoff;
typedef boost::intrusive_ptr<Backoff> BackoffRef;
class PG;
#ifdef PG_DEBUG_REFS
#include "common/tracked_int_ptr.hpp"
typedef TrackedIntPtr<PG> PGRef;
#else
typedef boost::intrusive_ptr<PG> PGRef;
#endif

/*
 * A Backoff represents one instance of either a PG or an OID
 * being plugged at the client.  It's refcounted and linked from
 * the PG {pg_oid}_backoffs map and from the client Session
 * object.
 *
 * The Backoff has a lock that protects it's internal fields.
 *
 * The PG has a backoff_lock that protects it's maps to Backoffs.
 * This lock is *inside* of Backoff::lock.
 *
 * The Session has a backoff_lock that protects it's map of pg and
 * oid backoffs.  This lock is *inside* the Backoff::lock *and*
 * PG::backoff_lock.
 *
 * That's
 *
 *    Backoff::lock
 *       PG::backoff_lock
 *         Session::backoff_lock
 *
 * When the Session goes away, we move our backoff lists aside,
 * then we lock each of the Backoffs we
 * previously referenced and clear the Session* pointer.  If the PG
 * is still linked, we unlink it, too.
 *
 * When the PG clears the backoff, it will send an unblock message
 * if the Session* is still non-null, and unlink the session.
 *
 */

struct Backoff : public RefCountedObject {
  enum {
    STATE_NEW = 1,     ///< backoff in flight to client
    STATE_ACKED = 2,   ///< backoff acked
    STATE_DELETING = 3 ///< backoff deleted, but un-acked
  };
  std::atomic<int> state = {STATE_NEW};
  spg_t pgid;          ///< owning pgid
  uint64_t id = 0;     ///< unique id (within the Session)

  bool is_new() const {
    return state.load() == STATE_NEW;
  }
  bool is_acked() const {
    return state.load() == STATE_ACKED;
  }
  bool is_deleting() const {
    return state.load() == STATE_DELETING;
  }
  const char *get_state_name() const {
    switch (state.load()) {
    case STATE_NEW: return "new";
    case STATE_ACKED: return "acked";
    case STATE_DELETING: return "deleting";
    default: return "???";
    }
  }

  Mutex lock;
  // NOTE: the owning PG and session are either
  //   - *both* set, or
  //   - both null (teardown), or
  //   - only session is set (and state == DELETING)
  PGRef pg;             ///< owning pg
  SessionRef session;   ///< owning session
  hobject_t begin, end; ///< [) range to block, unless ==, then single obj

  Backoff(spg_t pgid, PGRef pg, SessionRef s,
	  uint64_t i,
	  const hobject_t& b, const hobject_t& e)
    : RefCountedObject(g_ceph_context, 0),
      pgid(pgid),
      id(i),
      lock("Backoff::lock"),
      pg(pg),
      session(s),
      begin(b),
      end(e) {}

  friend ostream& operator<<(ostream& out, const Backoff& b) {
    return out << "Backoff(" << &b << " " << b.pgid << " " << b.id
	       << " " << b.get_state_name()
	       << " [" << b.begin << "," << b.end << ") "
	       << " session " << b.session
	       << " pg " << b.pg << ")";
  }
};



struct Session : public RefCountedObject {
  EntityName entity_name;
  OSDCap caps;
  ConnectionRef con;
  entity_addr_t socket_addr;
  WatchConState wstate;

  Mutex session_dispatch_lock;
  boost::intrusive::list<OpRequest> waiting_on_map;

  ceph::spinlock sent_epoch_lock;
  epoch_t last_sent_epoch;
  ceph::spinlock received_map_lock;
  epoch_t received_map_epoch; // largest epoch seen in MOSDMap from here

  /// protects backoffs; orders inside Backoff::lock *and* PG::backoff_lock
  Mutex backoff_lock;
  std::atomic<int> backoff_count= {0};  ///< simple count of backoffs
  map<spg_t,map<hobject_t,set<BackoffRef>>> backoffs;

  std::atomic<uint64_t> backoff_seq = {0};

  explicit Session(CephContext *cct, Connection *con_) :
    RefCountedObject(cct),
    con(con_),
    socket_addr(con_->get_peer_socket_addr()),
    wstate(cct),
    session_dispatch_lock("Session::session_dispatch_lock"),
    last_sent_epoch(0), received_map_epoch(0),
    backoff_lock("Session::backoff_lock")
    {}

  entity_addr_t& get_peer_socket_addr() {
    return socket_addr;
  }

  void ack_backoff(
    CephContext *cct,
    spg_t pgid,
    uint64_t id,
    const hobject_t& start,
    const hobject_t& end);

  BackoffRef have_backoff(spg_t pgid, const hobject_t& oid) {
    if (!backoff_count.load()) {
      return nullptr;
    }
    Mutex::Locker l(backoff_lock);
    ceph_assert(!backoff_count == backoffs.empty());
    auto i = backoffs.find(pgid);
    if (i == backoffs.end()) {
      return nullptr;
    }
    auto p = i->second.lower_bound(oid);
    if (p != i->second.begin() &&
	(p == i->second.end() || p->first > oid)) {
      --p;
    }
    if (p != i->second.end()) {
      int r = cmp(oid, p->first);
      if (r == 0 || r > 0) {
	for (auto& q : p->second) {
	  if (r == 0 || oid < q->end) {
	    return &(*q);
	  }
	}
      }
    }
    return nullptr;
  }

  bool check_backoff(
    CephContext *cct, spg_t pgid, const hobject_t& oid, const Message *m);

  void add_backoff(BackoffRef b) {
    Mutex::Locker l(backoff_lock);
    ceph_assert(!backoff_count == backoffs.empty());
    backoffs[b->pgid][b->begin].insert(b);
    ++backoff_count;
  }

  // called by PG::release_*_backoffs and PG::clear_backoffs()
  void rm_backoff(BackoffRef b) {
    Mutex::Locker l(backoff_lock);
    ceph_assert(b->lock.is_locked_by_me());
    ceph_assert(b->session == this);
    auto i = backoffs.find(b->pgid);
    if (i != backoffs.end()) {
      // may race with clear_backoffs()
      auto p = i->second.find(b->begin);
      if (p != i->second.end()) {
	auto q = p->second.find(b);
	if (q != p->second.end()) {
	  p->second.erase(q);
	  --backoff_count;
	  if (p->second.empty()) {
	    i->second.erase(p);
	    if (i->second.empty()) {
	      backoffs.erase(i);
	    }
	  }
	}
      }
    }
    ceph_assert(!backoff_count == backoffs.empty());
  }
  void clear_backoffs();
};

#endif
