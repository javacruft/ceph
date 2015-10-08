#ifndef CEPH_RGW_CR_RADOS_H
#define CEPH_RGW_CR_RADOS_H

#include "rgw_coroutine.h"
#include "common/WorkQueue.h"
#include "common/Throttle.h"

class RGWAsyncRadosRequest {
  RGWAioCompletionNotifier *notifier;

  void *user_info;
  int retcode;

protected:
  virtual int _send_request() = 0;
public:
  RGWAsyncRadosRequest(RGWAioCompletionNotifier *_cn) : notifier(_cn) {}
  virtual ~RGWAsyncRadosRequest() {}

  void send_request() {
    retcode = _send_request();
    notifier->cb();
  }

  int get_ret_status() { return retcode; }
};


class RGWAsyncRadosProcessor {
  deque<RGWAsyncRadosRequest *> m_req_queue;
protected:
  RGWRados *store;
  ThreadPool m_tp;
  Throttle req_throttle;

  struct RGWWQ : public ThreadPool::WorkQueue<RGWAsyncRadosRequest> {
    RGWAsyncRadosProcessor *processor;
    RGWWQ(RGWAsyncRadosProcessor *p, time_t timeout, time_t suicide_timeout, ThreadPool *tp)
      : ThreadPool::WorkQueue<RGWAsyncRadosRequest>("RGWWQ", timeout, suicide_timeout, tp), processor(p) {}

    bool _enqueue(RGWAsyncRadosRequest *req);
    void _dequeue(RGWAsyncRadosRequest *req) {
      assert(0);
    }
    bool _empty();
    RGWAsyncRadosRequest *_dequeue();
    using ThreadPool::WorkQueue<RGWAsyncRadosRequest>::_process;
    void _process(RGWAsyncRadosRequest *req);
    void _dump_queue();
    void _clear() {
      assert(processor->m_req_queue.empty());
    }
  } req_wq;

public:
  RGWAsyncRadosProcessor(RGWRados *_store, int num_threads);
  ~RGWAsyncRadosProcessor() {}
  void start();
  void stop();
  void handle_request(RGWAsyncRadosRequest *req);
  void queue(RGWAsyncRadosRequest *req);
};


class RGWAsyncGetSystemObj : public RGWAsyncRadosRequest {
  RGWRados *store;
  RGWObjectCtx *obj_ctx;
  RGWRados::SystemObject::Read::GetObjState read_state;
  RGWObjVersionTracker *objv_tracker;
  rgw_obj obj;
  bufferlist *pbl;
  map<string, bufferlist> *pattrs;
  off_t ofs;
  off_t end;
protected:
  int _send_request();
public:
  RGWAsyncGetSystemObj(RGWAioCompletionNotifier *cn, RGWRados *_store, RGWObjectCtx *_obj_ctx,
                       RGWObjVersionTracker *_objv_tracker, rgw_obj& _obj,
                       bufferlist *_pbl, off_t _ofs, off_t _end);
  void set_read_attrs(map<string, bufferlist> *_pattrs) { pattrs = _pattrs; }
};

class RGWAsyncPutSystemObj : public RGWAsyncRadosRequest {
  RGWRados *store;
  RGWObjVersionTracker *objv_tracker;
  rgw_obj obj;
  bool exclusive;
  bufferlist bl;
  map<string, bufferlist> attrs;
  time_t mtime;

protected:
  int _send_request();
public:
  RGWAsyncPutSystemObj(RGWAioCompletionNotifier *cn, RGWRados *_store,
                       RGWObjVersionTracker *_objv_tracker, rgw_obj& _obj, bool _exclusive,
                       bufferlist& _bl, time_t _mtime = 0);
};

class RGWAsyncPutSystemObjAttrs : public RGWAsyncRadosRequest {
  RGWRados *store;
  RGWObjVersionTracker *objv_tracker;
  rgw_obj obj;
  map<string, bufferlist> *attrs;

protected:
  int _send_request();
public:
  RGWAsyncPutSystemObjAttrs(RGWAioCompletionNotifier *cn, RGWRados *_store,
                       RGWObjVersionTracker *_objv_tracker, rgw_obj& _obj,
                       map<string, bufferlist> *_attrs);
};

class RGWAsyncLockSystemObj : public RGWAsyncRadosRequest {
  RGWRados *store;
  rgw_obj obj;
  string lock_name;
  string cookie;
  uint32_t duration_secs;

protected:
  int _send_request();
public:
  RGWAsyncLockSystemObj(RGWAioCompletionNotifier *cn, RGWRados *_store,
                        RGWObjVersionTracker *_objv_tracker, rgw_obj& _obj,
		        const string& _name, const string& _cookie, uint32_t _duration_secs);
};

class RGWAsyncUnlockSystemObj : public RGWAsyncRadosRequest {
  RGWRados *store;
  rgw_obj obj;
  string lock_name;
  string cookie;

protected:
  int _send_request();
public:
  RGWAsyncUnlockSystemObj(RGWAioCompletionNotifier *cn, RGWRados *_store,
                        RGWObjVersionTracker *_objv_tracker, rgw_obj& _obj,
		        const string& _name, const string& _cookie);
};


template <class T>
class RGWSimpleRadosReadCR : public RGWSimpleCoroutine {
  RGWAsyncRadosProcessor *async_rados;
  RGWRados *store;
  RGWObjectCtx& obj_ctx;
  bufferlist bl;

  rgw_bucket pool;
  string oid;

  map<string, bufferlist> *pattrs;

  T *result;

  RGWAsyncGetSystemObj *req;

public:
  RGWSimpleRadosReadCR(RGWAsyncRadosProcessor *_async_rados, RGWRados *_store,
		      RGWObjectCtx& _obj_ctx,
		      rgw_bucket& _pool, const string& _oid,
		      T *_result) : RGWSimpleCoroutine(_store->ctx()),
                                                async_rados(_async_rados), store(_store),
                                                obj_ctx(_obj_ctx),
						pool(_pool), oid(_oid),
                                                pattrs(NULL),
						result(_result),
                                                req(NULL) { }
                                                         
  ~RGWSimpleRadosReadCR() {
    delete req;
  }

  int send_request();
  int request_complete();

  virtual int handle_data(T& data) {
    return 0;
  }
};

template <class T>
int RGWSimpleRadosReadCR<T>::send_request()
{
  rgw_obj obj = rgw_obj(pool, oid);
  req = new RGWAsyncGetSystemObj(stack->create_completion_notifier(),
			         store, &obj_ctx, NULL,
				 obj,
				 &bl, 0, -1);
  if (pattrs) {
    req->set_read_attrs(pattrs);
  }
  async_rados->queue(req);
  return 0;
}

template <class T>
int RGWSimpleRadosReadCR<T>::request_complete()
{
  int ret = req->get_ret_status();
  retcode = ret;
  if (ret != -ENOENT) {
    if (ret < 0) {
      return ret;
    }
    bufferlist::iterator iter = bl.begin();
    try {
      ::decode(*result, iter);
    } catch (buffer::error& err) {
      return -EIO;
    }
  } else {
    *result = T();
  }

  return handle_data(*result);
}

class RGWSimpleRadosReadAttrsCR : public RGWSimpleCoroutine {
  RGWAsyncRadosProcessor *async_rados;
  RGWRados *store;
  RGWObjectCtx& obj_ctx;
  bufferlist bl;

  rgw_bucket pool;
  string oid;

  map<string, bufferlist> *pattrs;

  RGWAsyncGetSystemObj *req;

public:
  RGWSimpleRadosReadAttrsCR(RGWAsyncRadosProcessor *_async_rados, RGWRados *_store,
		      RGWObjectCtx& _obj_ctx,
		      rgw_bucket& _pool, const string& _oid,
		      map<string, bufferlist> *_pattrs) : RGWSimpleCoroutine(_store->ctx()),
                                                async_rados(_async_rados), store(_store),
                                                obj_ctx(_obj_ctx),
						pool(_pool), oid(_oid),
                                                pattrs(_pattrs),
                                                req(NULL) { }
                                                         
  ~RGWSimpleRadosReadAttrsCR() {
    delete req;
  }

  int send_request();
  int request_complete();
};

template <class T>
class RGWSimpleRadosWriteCR : public RGWSimpleCoroutine {
  RGWAsyncRadosProcessor *async_rados;
  RGWRados *store;
  bufferlist bl;

  rgw_bucket pool;
  string oid;

  RGWAsyncPutSystemObj *req;

public:
  RGWSimpleRadosWriteCR(RGWAsyncRadosProcessor *_async_rados, RGWRados *_store,
		      rgw_bucket& _pool, const string& _oid,
		      const T& _data) : RGWSimpleCoroutine(_store->ctx()),
                                                async_rados(_async_rados),
						store(_store),
						pool(_pool), oid(_oid),
                                                req(NULL) {
    ::encode(_data, bl);
  }

  ~RGWSimpleRadosWriteCR() {
    delete req;
  }

  int send_request() {
    rgw_obj obj = rgw_obj(pool, oid);
    req = new RGWAsyncPutSystemObj(stack->create_completion_notifier(),
			           store, NULL, obj, false, bl);
    async_rados->queue(req);
    return 0;
  }

  int request_complete() {
    return req->get_ret_status();
  }
};

class RGWSimpleRadosWriteAttrsCR : public RGWSimpleCoroutine {
  RGWAsyncRadosProcessor *async_rados;
  RGWRados *store;

  rgw_bucket pool;
  string oid;

  map<string, bufferlist> attrs;

  RGWAsyncPutSystemObjAttrs *req;

public:
  RGWSimpleRadosWriteAttrsCR(RGWAsyncRadosProcessor *_async_rados, RGWRados *_store,
		      rgw_bucket& _pool, const string& _oid,
		      map<string, bufferlist>& _attrs) : RGWSimpleCoroutine(_store->ctx()),
                                                async_rados(_async_rados),
						store(_store),
						pool(_pool), oid(_oid),
                                                attrs(_attrs) {
  }

  ~RGWSimpleRadosWriteAttrsCR() {
    delete req;
  }

  int send_request() {
    rgw_obj obj = rgw_obj(pool, oid);
    req = new RGWAsyncPutSystemObjAttrs(stack->create_completion_notifier(),
			           store, NULL, obj, &attrs);
    async_rados->queue(req);
    return 0;
  }

  int request_complete() {
    return req->get_ret_status();
  }
};

class RGWRadosSetOmapKeysCR : public RGWSimpleCoroutine {
  RGWRados *store;
  map<string, bufferlist> entries;

  rgw_bucket pool;
  string oid;

  RGWAioCompletionNotifier *cn;

public:
  RGWRadosSetOmapKeysCR(RGWRados *_store,
		      rgw_bucket& _pool, const string& _oid,
		      map<string, bufferlist>& _entries);
  ~RGWRadosSetOmapKeysCR();

  int send_request();
  int request_complete();
};

class RGWRadosGetOmapKeysCR : public RGWSimpleCoroutine {
  RGWRados *store;

  string marker;
  map<string, bufferlist> *entries;
  int max_entries;

  int rval;
  librados::IoCtx ioctx;

  rgw_bucket pool;
  string oid;

  RGWAioCompletionNotifier *cn;

public:
  RGWRadosGetOmapKeysCR(RGWRados *_store,
		      rgw_bucket& _pool, const string& _oid,
		      const string& _marker,
		      map<string, bufferlist> *_entries, int _max_entries);
  ~RGWRadosGetOmapKeysCR();

  int send_request();

  int request_complete() {
    return rval;
  }
};

class RGWSimpleRadosLockCR : public RGWSimpleCoroutine {
  RGWAsyncRadosProcessor *async_rados;
  RGWRados *store;
  string lock_name;
  string cookie;
  uint32_t duration;

  rgw_bucket pool;
  string oid;

  RGWAsyncLockSystemObj *req;

public:
  RGWSimpleRadosLockCR(RGWAsyncRadosProcessor *_async_rados, RGWRados *_store,
		      rgw_bucket& _pool, const string& _oid, const string& _lock_name,
		      const string& _cookie,
		      uint32_t _duration);
  ~RGWSimpleRadosLockCR();

  int send_request();
  int request_complete();
};

class RGWSimpleRadosUnlockCR : public RGWSimpleCoroutine {
  RGWAsyncRadosProcessor *async_rados;
  RGWRados *store;
  string lock_name;
  string cookie;

  rgw_bucket pool;
  string oid;

  RGWAsyncUnlockSystemObj *req;

public:
  RGWSimpleRadosUnlockCR(RGWAsyncRadosProcessor *_async_rados, RGWRados *_store,
		      rgw_bucket& _pool, const string& _oid, const string& _lock_name,
		      const string& _cookie);
  ~RGWSimpleRadosUnlockCR();

  int send_request();
  int request_complete();
};

class RGWOmapAppend : public RGWConsumerCR<string> {
  RGWAsyncRadosProcessor *async_rados;
  RGWRados *store;

  rgw_bucket pool;
  string oid;

  bool going_down;

  int num_pending_entries;
  list<string> pending_entries;

  map<string, bufferlist> entries;
public:
  RGWOmapAppend(RGWAsyncRadosProcessor *_async_rados, RGWRados *_store, rgw_bucket& _pool, const string& _oid);
  int operate();
  void flush_pending();
  void append(const string& s);
  void finish();
};

class RGWAsyncWait : public RGWAsyncRadosRequest {
  CephContext *cct;
  Mutex *lock;
  Cond *cond;
  utime_t interval;
protected:
  int _send_request() {
    Mutex::Locker l(*lock);
    return cond->WaitInterval(cct, *lock, interval);
  }
public:
  RGWAsyncWait(RGWAioCompletionNotifier *cn, CephContext *_cct, Mutex *_lock, Cond *_cond, int _secs) : RGWAsyncRadosRequest(cn),
                                       cct(_cct),
                                       lock(_lock), cond(_cond), interval(_secs, 0) {}

  void wakeup() {
    Mutex::Locker l(*lock);
    cond->Signal();
  }
};

class RGWWaitCR : public RGWSimpleCoroutine {
  CephContext *cct;
  RGWAsyncRadosProcessor *async_rados;
  Mutex *lock;
  Cond *cond;
  int secs;

  RGWAsyncWait *req;

public:
  RGWWaitCR(RGWAsyncRadosProcessor *_async_rados, CephContext *_cct,
	    Mutex *_lock, Cond *_cond,
            int _secs) : RGWSimpleCoroutine(cct), cct(_cct),
                         async_rados(_async_rados), lock(_lock), cond(_cond), secs(_secs) {
  }

  ~RGWWaitCR() {
    wakeup();
    delete req;
  }

  int send_request() {
    req = new RGWAsyncWait(stack->create_completion_notifier(), cct,  lock, cond, secs);
    async_rados->queue(req);
    return 0;
  }

  int request_complete() {
    return req->get_ret_status();
  }

  void wakeup() {
    req->wakeup();
  }
};

class RGWShardedOmapCRManager {
  RGWAsyncRadosProcessor *async_rados;
  RGWRados *store;
  RGWCoroutine *op;

  int num_shards;

  vector<RGWOmapAppend *> shards;
public:
  RGWShardedOmapCRManager(RGWAsyncRadosProcessor *_async_rados, RGWRados *_store, RGWCoroutine *_op, int _num_shards, rgw_bucket& pool, const string& oid_prefix)
                      : async_rados(_async_rados),
		        store(_store), op(_op), num_shards(_num_shards) {
    shards.reserve(num_shards);
    for (int i = 0; i < num_shards; ++i) {
      char buf[oid_prefix.size() + 16];
      snprintf(buf, sizeof(buf), "%s.%d", oid_prefix.c_str(), i);
      RGWOmapAppend *shard = new RGWOmapAppend(async_rados, store, pool, buf);
      shards.push_back(shard);
      op->spawn(shard, false);
    }
  }
  void append(const string& entry) {
    int shard_id = store->key_to_shard_id(entry, shards.size());
    shards[shard_id]->append(entry);
  }
  void finish() {
    for (vector<RGWOmapAppend *>::iterator iter = shards.begin(); iter != shards.end(); ++iter) {
      (*iter)->finish();
    }
  }
};

class RGWAsyncGetBucketInstanceInfo : public RGWAsyncRadosRequest {
  RGWRados *store;
  string bucket_name;
  string bucket_id;
  RGWBucketInfo *bucket_info;

protected:
  int _send_request();
public:
  RGWAsyncGetBucketInstanceInfo(RGWAioCompletionNotifier *cn, RGWRados *_store,
		        const string& _bucket_name, const string& _bucket_id,
                        RGWBucketInfo *_bucket_info) : RGWAsyncRadosRequest(cn), store(_store),
                                                       bucket_name(_bucket_name), bucket_id(_bucket_id),
                                                       bucket_info(_bucket_info) {}
};

class RGWGetBucketInstanceInfoCR : public RGWSimpleCoroutine {
  RGWAsyncRadosProcessor *async_rados;
  RGWRados *store;
  string bucket_name;
  string bucket_id;
  RGWBucketInfo *bucket_info;

  RGWAsyncGetBucketInstanceInfo *req;
  
public:
  RGWGetBucketInstanceInfoCR(RGWAsyncRadosProcessor *_async_rados, RGWRados *_store,
		        const string& _bucket_name, const string& _bucket_id,
                        RGWBucketInfo *_bucket_info) : RGWSimpleCoroutine(_store->ctx()), async_rados(_async_rados), store(_store),
                                                       bucket_name(_bucket_name), bucket_id(_bucket_id),
                                                       bucket_info(_bucket_info), req(NULL) {}
  ~RGWGetBucketInstanceInfoCR() {
    delete req;
  }

  int send_request() {
    req = new RGWAsyncGetBucketInstanceInfo(stack->create_completion_notifier(), store, bucket_name, bucket_id, bucket_info);
    async_rados->queue(req);
    return 0;
  }
  int request_complete() {
    return req->get_ret_status();
  }
};

class RGWAsyncFetchRemoteObj : public RGWAsyncRadosRequest {
  RGWRados *store;
  string source_zone;

  RGWBucketInfo bucket_info;

  rgw_obj_key key;
  uint64_t versioned_epoch;

  time_t src_mtime;

  bool copy_if_newer;

protected:
  int _send_request();
public:
  RGWAsyncFetchRemoteObj(RGWAioCompletionNotifier *cn, RGWRados *_store,
                         const string& _source_zone,
                         RGWBucketInfo& _bucket_info,
                         const rgw_obj_key& _key,
                         uint64_t _versioned_epoch,
                         bool _if_newer) : RGWAsyncRadosRequest(cn), store(_store),
                                                      source_zone(_source_zone),
                                                      bucket_info(_bucket_info),
                                                      key(_key),
                                                      versioned_epoch(_versioned_epoch),
                                                      copy_if_newer(_if_newer) {}
};

class RGWFetchRemoteObjCR : public RGWSimpleCoroutine {
  CephContext *cct;
  RGWAsyncRadosProcessor *async_rados;
  RGWRados *store;
  string source_zone;

  RGWBucketInfo bucket_info;

  rgw_obj_key key;
  uint64_t versioned_epoch;

  time_t src_mtime;

  bool copy_if_newer;

  RGWAsyncFetchRemoteObj *req;

public:
  RGWFetchRemoteObjCR(RGWAsyncRadosProcessor *_async_rados, RGWRados *_store,
                      const string& _source_zone,
                      RGWBucketInfo& _bucket_info,
                      const rgw_obj_key& _key,
                      uint64_t _versioned_epoch,
                      bool _if_newer) : RGWSimpleCoroutine(_store->ctx()), cct(_store->ctx()),
                                       async_rados(_async_rados), store(_store),
                                       source_zone(_source_zone),
                                       bucket_info(_bucket_info),
                                       key(_key),
                                       versioned_epoch(_versioned_epoch),
                                       copy_if_newer(_if_newer), req(NULL) {}


  ~RGWFetchRemoteObjCR() {
    delete req;
  }

  int send_request() {
    req = new RGWAsyncFetchRemoteObj(stack->create_completion_notifier(), store, source_zone, bucket_info,
                                     key, versioned_epoch, copy_if_newer);
    async_rados->queue(req);
    return 0;
  }

  int request_complete() {
    return req->get_ret_status();
  }
};

class RGWAsyncRemoveObj : public RGWAsyncRadosRequest {
  RGWRados *store;
  string source_zone;

  RGWBucketInfo bucket_info;

  rgw_obj_key key;
  uint64_t versioned_epoch;

  bool del_if_older;
  utime_t timestamp;

protected:
  int _send_request();
public:
  RGWAsyncRemoveObj(RGWAioCompletionNotifier *cn, RGWRados *_store,
                         const string& _source_zone,
                         RGWBucketInfo& _bucket_info,
                         const rgw_obj_key& _key,
                         uint64_t _versioned_epoch,
                         bool _if_older,
                         utime_t& _timestamp) : RGWAsyncRadosRequest(cn), store(_store),
                                                      source_zone(_source_zone),
                                                      bucket_info(_bucket_info),
                                                      key(_key),
                                                      versioned_epoch(_versioned_epoch),
                                                      del_if_older(_if_older),
                                                      timestamp(_timestamp) {}
};

class RGWRemoveObjCR : public RGWSimpleCoroutine {
  CephContext *cct;
  RGWAsyncRadosProcessor *async_rados;
  RGWRados *store;
  string source_zone;

  RGWBucketInfo bucket_info;

  rgw_obj_key key;
  uint64_t versioned_epoch;

  bool del_if_older;
  utime_t timestamp;

  RGWAsyncRemoveObj *req;

public:
  RGWRemoveObjCR(RGWAsyncRadosProcessor *_async_rados, RGWRados *_store,
                      const string& _source_zone,
                      RGWBucketInfo& _bucket_info,
                      const rgw_obj_key& _key,
                      uint64_t _versioned_epoch,
                      utime_t *_timestamp) : RGWSimpleCoroutine(_store->ctx()), cct(_store->ctx()),
                                       async_rados(_async_rados), store(_store),
                                       source_zone(_source_zone),
                                       bucket_info(_bucket_info),
                                       key(_key),
                                       versioned_epoch(_versioned_epoch) {
    del_if_older = (_timestamp != NULL);
    if (_timestamp) {
      timestamp = *_timestamp;
    }
  }

  ~RGWRemoveObjCR() {
    delete req;
  }

  int send_request() {
    req = new RGWAsyncRemoveObj(stack->create_completion_notifier(), store, source_zone, bucket_info,
                                key, versioned_epoch, del_if_older, timestamp);
    async_rados->queue(req);
    return 0;
  }

  int request_complete() {
    return req->get_ret_status();
  }
};

#endif
