// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "rgw_frontend.h"

#include "include/str_list.h"

static int civetweb_callback(struct mg_connection *conn) {
  struct mg_request_info *req_info = mg_get_request_info(conn);
  RGWProcessEnv *pe = static_cast<RGWProcessEnv *>(req_info->user_data);
  RGWRados *store = pe->store;
  RGWREST *rest = pe->rest;
  OpsLogSocket *olog = pe->olog;

  RGWRequest *req = new RGWRequest(store->get_new_req_id());
  RGWMongoose client_io(conn, pe->port);

  int ret = process_request(store, rest, req, &client_io, olog);
  if (ret < 0) {
    /* we don't really care about return code */
    dout(20) << "process_request() returned " << ret << dendl;
  }

  delete req;

// Mark as processed
  return 1;
}

int RGWMongooseFrontend::run()
{
    char thread_pool_buf[32];
    snprintf(thread_pool_buf, sizeof(thread_pool_buf), "%d",
	     (int)g_conf->rgw_thread_pool_size);
    string port_str;
    map<string, string> conf_map = conf->get_config_map();
    conf->get_val("port", "80", &port_str);
    conf_map.erase("port");
    conf_map["listening_ports"] = port_str;
    set_conf_default(conf_map, "enable_keep_alive", "yes");
    set_conf_default(conf_map, "num_threads", thread_pool_buf);
    set_conf_default(conf_map, "decode_url", "no");

    const char *options[conf_map.size() * 2 + 1];
    int i = 0;
    for (map<string, string>::iterator iter = conf_map.begin();
	 iter != conf_map.end(); ++iter) {
	options[i] = iter->first.c_str();
	options[i + 1] = iter->second.c_str();
	dout(20)<< "civetweb config: " << options[i] << ": "
		<< (options[i + 1] ? options[i + 1] : "<null>") << dendl;
	i += 2;
    }
    options[i] = NULL;

    struct mg_callbacks cb;
    memset((void *)&cb, 0, sizeof(cb));
    cb.begin_request = civetweb_callback;
    cb.log_message = rgw_civetweb_log_callback;
    cb.log_access = rgw_civetweb_log_access_callback;
    ctx = mg_start(&cb, &env, (const char **)&options);

    if (!ctx) {
	return -EIO;
    }

    return 0;
} /* RGWMongooseFrontend::run() */
