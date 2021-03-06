/*
 * Copyright (c) 2015, Circonus, Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *    * Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *    * Redistributions in binary form must reproduce the above
 *      copyright notice, this list of conditions and the following
 *      disclaimer in the documentation and/or other materials provided
 *      with the distribution.
 *    * Neither the name Circonus, Inc. nor the names of its contributors
 *      may be used to endorse or promote products derived from this
 *      software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "mtev_defines.h"

#include <assert.h>
#include <libxml/tree.h>

#include "mtev_rest.h"
#include "mtev_conf.h"
#include "mtev_hash.h"
#include "mtev_memory.h"
#include "mtev_cluster.h"
#include "mtev_cht.h"

static pthread_mutex_t c_lock = PTHREAD_MUTEX_INITIALIZER;
static uuid_t my_cluster_id;
static mtev_boolean have_clusters;
static mtev_hash_table global_clusters;

/* All allocated with mtev_memory_safe commands */
struct mtev_cluster_t {
  char *name;
  int64_t config_seq;
  int node_cnt;
  mtev_cluster_node_t *nodes;
  mtev_cht_t **cht;
};

mtev_boolean
mtev_cluster_enabled() {
  return have_clusters;
}

static void
mtev_cluster_free(void *vc) {
  mtev_cluster_t *c = vc;
  if(c) {
    if(c->name) mtev_memory_safe_free(c->name);
    if(c->nodes) mtev_memory_safe_free(c->nodes);
    if(c->cht) mtev_memory_safe_free(c->cht);
    mtev_memory_safe_free(c);
  }
}

/* the CHT doesn't use safe reclamation, so we wrap it here */
static void
deferred_cht_free(void *vptr) {
  mtev_cht_t **chtp;
  if(!vptr) return;
  if(*chtp) mtev_cht_free(*chtp);
  free(vptr);
}

static void
mtev_cluster_compile(mtev_cluster_t *cluster) {
  int i;
  mtev_cht_node_t *nodes;

  if(cluster->cht) mtev_memory_safe_free(cluster->cht);
  if(cluster->node_cnt == 0) {
    cluster->cht = NULL;
    return;
  }
  cluster->cht =
    mtev_memory_safe_malloc_cleanup(sizeof(*cluster->cht),
                                    deferred_cht_free);
  *(cluster->cht) = mtev_cht_alloc();
  nodes = calloc(sizeof(*nodes), cluster->node_cnt);
  for(i=0; i<cluster->node_cnt; i++) {
    char uuid_str[UUID_STR_LEN+1];
    uuid_unparse_lower(cluster->nodes[i].id, uuid_str);
    nodes->name = strdup(uuid_str);
    nodes->userdata = &cluster->nodes[i];
  }
  mtev_cht_set_nodes(*(cluster->cht), cluster->node_cnt, nodes);
}

static int
mtev_cluster_update_config(mtev_cluster_t *cluster, mtev_boolean create) {
  int i;
  char xpath_search[256], new_seq_str[32];
  xmlNodePtr container = NULL, parent = NULL;
  mtev_conf_section_t n;

  snprintf(xpath_search, sizeof(xpath_search),
           "//clusters/cluster[@name=\"%s\"]", cluster->name);
  n = mtev_conf_get_section(NULL, xpath_search);
  parent = (xmlNodePtr)n;

  snprintf(new_seq_str, sizeof(new_seq_str), "%lld", cluster->config_seq);
  if(!create) {
    xmlNodePtr child;
    if(!parent) return 0;
    xmlUnsetProp(parent, (xmlChar *)"seq");
    xmlSetProp(parent, (xmlChar *)"seq", (xmlChar *)new_seq_str);
    while(NULL != (child = parent->children)) {
      xmlUnlinkNode(child);
      xmlFreeNode(child);
    }
  }
  else {
    if(parent) return 0;
    n = mtev_conf_get_section(NULL, "//clusters");
    if(!n) {
      mtevL(mtev_error, "Cluster config attempted with no 'clusters' section.\n");
      return 0;
    }
    container = (xmlNodePtr)n;
    parent = xmlNewNode(NULL, (xmlChar *)"cluster");
    xmlSetProp(parent, (xmlChar *)"name", (xmlChar *)cluster->name);
    xmlSetProp(parent, (xmlChar *)"seq", (xmlChar *)new_seq_str);
  }
  for(i=0;i<cluster->node_cnt;i++) {
    xmlNodePtr node;
    char uuid_str[UUID_STR_LEN+1], port[6], ipstr[INET6_ADDRSTRLEN];
    node = xmlNewNode(NULL, (xmlChar *)"node");
    uuid_unparse_lower(cluster->nodes[i].id, uuid_str);
    xmlSetProp(node, (xmlChar *)"id", (xmlChar *)uuid_str);
    xmlSetProp(node, (xmlChar *)"cn", (xmlChar *)cluster->nodes[i].cn);
    if(cluster->nodes[i].addr.addr4.sin_family == AF_INET) {
      inet_ntop(AF_INET, &cluster->nodes[i].addr.addr4.sin_addr,
                ipstr, sizeof(ipstr));
      xmlSetProp(node, (xmlChar *)"address", (xmlChar *)ipstr);
      snprintf(port, sizeof(port), "%d", ntohs(cluster->nodes[i].addr.addr4.sin_port));
      xmlSetProp(node, (xmlChar *)"port", (xmlChar *)port);
    }
    else if(cluster->nodes[i].addr.addr6.sin6_family == AF_INET6) {
      inet_ntop(AF_INET6, &cluster->nodes[i].addr.addr6.sin6_addr,
                ipstr, sizeof(ipstr));
      xmlSetProp(node, (xmlChar *)"address", (xmlChar *)ipstr);
      snprintf(port, sizeof(port), "%d", ntohs(cluster->nodes[i].addr.addr6.sin6_port));
      xmlSetProp(node, (xmlChar *)"port", (xmlChar *)port);
    }
    xmlAddChild(parent, node);
  }
  if(container) xmlAddChild(container, parent);

  /* Dirty the right nodes */
  if(container) CONF_DIRTY(container);
  else CONF_DIRTY(parent);
  mtev_conf_mark_changed();
  mtev_conf_request_write();

  return 1;
 bail:
  if(create && parent) xmlFree(parent);
  return 0;
}

int
mtev_cluster_update_internal(mtev_conf_section_t cluster,
                             mtev_boolean booted) {
  int rv = -1, i, n_nodes;
  int64_t seq;
  char bufstr[1024];
  mtev_conf_section_t *nodes = NULL;
  char *name = NULL, *endptr;
  void *vcluster;
  mtev_cluster_t *old_cluster = NULL;
  mtev_cluster_t *new_cluster = NULL;
  mtev_cluster_node_t *nlist = NULL;

  if(!mtev_conf_get_stringbuf(cluster, "@name", bufstr, sizeof(bufstr))) {
    mtevL(mtev_error, "Cluster has no name, skipping.\n");
    goto bail;
  }
  name = mtev_memory_safe_strdup(bufstr);

  if(!mtev_conf_get_stringbuf(cluster, "@seq", bufstr, sizeof(bufstr)) ||
     bufstr[0] == '\0') {
    mtevL(mtev_error, "Cluster '%s' has no seq, skipping.\n", name);
    goto bail;
  }
  seq = strtoll(bufstr, &endptr, 10);
  if(*endptr) {
    mtevL(mtev_error, "Cluster '%s' seq invalid.\n", name);
    goto bail;
  }

  nodes = mtev_conf_get_sections(cluster, "node", &n_nodes);
  if(n_nodes > 0) {
    nlist = mtev_memory_safe_calloc(n_nodes, sizeof(*nlist));
    for(i=0;i<n_nodes;i++) {
      int family;
      int port;
      union {
        struct in_addr addr4;
        struct in6_addr addr6;
      } a;
      char uuid_str[UUID_STR_LEN+1];

      if(!mtev_conf_get_stringbuf(nodes[i], "@id", uuid_str, sizeof(uuid_str)) ||
         uuid_parse(uuid_str, nlist[i].id) != 0) {
        mtevL(mtev_error, "Cluster '%s' node %d has no (or bad) id\n", name, i);
        goto bail;
      }
      if(!mtev_conf_get_stringbuf(nodes[i], "@cn",
        nlist[i].cn, sizeof(nlist[i].cn))) {
        mtevL(mtev_error, "Cluster '%s' node %d has no cn\n", name, i);
        goto bail;
      }
      if(!mtev_conf_get_int(nodes[i], "@port", &port) || port < 0 || port > 0xffff) {
        mtevL(mtev_error, "Cluster '%s' node %d has no (or bad) port\n", name, i);
        goto bail;
      }
      if(!mtev_conf_get_stringbuf(nodes[i], "@address", bufstr, sizeof(bufstr))) {
        mtevL(mtev_error, "Cluster '%s' node %d has no address\n", name, i);
        goto bail;
      }
      
      family = AF_INET;
      rv = inet_pton(family, bufstr, &a);
      if(rv != 1) {
        family = AF_INET6;
        rv = inet_pton(family, bufstr, &a);
        if(rv != 1) {
          mtevL(mtev_error, "Cluster '%s' node '%s' has bad address '%s'\n",
                name, uuid_str, bufstr);
          goto bail;
        }
        else {
          nlist[i].addr.addr6.sin6_family = AF_INET6;
          nlist[i].addr.addr6.sin6_addr = a.addr6;
          nlist[i].addr.addr6.sin6_port = htons((unsigned short)port);
          nlist[i].address_len = sizeof(nlist[i].addr.addr6);
        }
      }
      else {
        nlist[i].addr.addr4.sin_family = AF_INET;
        nlist[i].addr.addr4.sin_addr = a.addr4;
        nlist[i].addr.addr4.sin_port = htons((unsigned short)port);
        nlist[i].address_len = sizeof(nlist[i].addr.addr4);
      }
    }
  }

  new_cluster = mtev_memory_safe_calloc(1, sizeof(*new_cluster));
  new_cluster->name = name; name = NULL;
  new_cluster->config_seq = seq;
  new_cluster->node_cnt = n_nodes;
  new_cluster->nodes = nlist; nlist = NULL;

  pthread_mutex_lock(&c_lock);
  if(mtev_hash_retrieve(&global_clusters,
                        new_cluster->name, strlen(new_cluster->name),
                        &vcluster)) {
    old_cluster = vcluster;
    rv = 1;
    /* Validate sequence bump */
    if(new_cluster->config_seq <= old_cluster->config_seq) {
      /* This is considered a successful update. We have the most recent */
      rv = 2;
      mtevL(mtev_debug, "Cluster '%s' is too old\n", new_cluster->name);
      goto bail;
    }
    if(!mtev_cluster_update_config(new_cluster, mtev_false)) {
      mtevL(mtev_error, "Cluster '%s', failed to write to config.\n",
            new_cluster->name);
      rv = -1;
      goto bail;
    }
    mtev_cluster_compile(new_cluster);
    mtev_hash_replace(&global_clusters,
	      new_cluster->name, strlen(new_cluster->name),
                      new_cluster, NULL, mtev_cluster_free);
  }
  else {
    if(!mtev_cluster_update_config(new_cluster, booted && mtev_true)) {
      mtevL(mtev_error, "Cluster '%s', failed to write to config.\n",
            new_cluster->name);
      rv = -1;
      goto bail;
    }
    mtev_cluster_compile(new_cluster);
    mtev_hash_store(&global_clusters,
                    new_cluster->name, strlen(new_cluster->name),
                    new_cluster);
    rv = 0;
  }
  new_cluster = NULL;
  pthread_mutex_unlock(&c_lock);

 bail:
  if(nodes) free(nodes);
  if(name) mtev_memory_safe_free(name);
  if(new_cluster) mtev_memory_safe_free(new_cluster);
  if(nlist) mtev_memory_safe_free(nlist);
  return rv;
}

int
mtev_cluster_update(mtev_conf_section_t cluster) {
  return mtev_cluster_update_internal(cluster, mtev_true);
}

mtev_cluster_t *
mtev_cluster_by_name(const char *name) {
  void *vc;
  if(mtev_hash_retrieve(&global_clusters, name, strlen(name), &vc))
    return (mtev_cluster_t *)vc;
  return NULL;
}

int
mtev_cluster_size(mtev_cluster_t *c) {
  return c ? c->node_cnt : 0;
}

void
mtev_cluster_set_self(uuid_t id) {
  char old_uuid[UUID_STR_LEN+1],
       my_id_str[UUID_STR_LEN+1];
  uuid_copy(my_cluster_id, id);
  uuid_unparse_lower(my_cluster_id, my_id_str);
  if(!mtev_conf_get_stringbuf(NULL, "//clusters/@my_id",
                              old_uuid, sizeof(old_uuid)) ||
     strcmp(old_uuid, my_id_str)) {
    mtev_conf_section_t c;
    c = mtev_conf_get_section(NULL, "//clusters");
    if(c) {
      xmlUnsetProp((xmlNodePtr)c, (xmlChar *)"my_id");
      xmlSetProp((xmlNodePtr)c, (xmlChar *)"my_id", (xmlChar *)my_id_str);
      CONF_DIRTY(c);
      mtev_conf_mark_changed();
      mtev_conf_request_write();
    }
  }
}
void
mtev_cluster_get_self(uuid_t id) {
  uuid_copy(id, my_cluster_id);
}

int
mtev_cluster_get_nodes(mtev_cluster_t *c,
                       mtev_cluster_node_t **nodes, int n,
                       mtev_boolean includeme) {
  int i, o = 0;
  if(!c) return 0;
  for(i=0;i<n && i<c->node_cnt; i++) {
    if(includeme || uuid_compare(c->nodes[i].id, my_cluster_id)) {
      nodes[o++] = &c->nodes[i];
    }
  }
  return o;
}

mtev_boolean
mtev_cluster_do_i_own(mtev_cluster_t *c, void *key, size_t klen, int w) {
  int i, wout;
  mtev_cht_node_t **owners;
  if(!c || !c->cht || !(*(c->cht))) return mtev_false;
  if(w < 0) w = 1;
  if(w > c->node_cnt) w = c->node_cnt;
  owners = alloca(sizeof(*owners) * c->node_cnt);
  wout = mtev_cht_vlookup_n(*(c->cht), key, klen, w, owners);
  for(i=0; i<wout; i++) {
    mtev_cluster_node_t *node;
    node = owners[i]->userdata;
    if(uuid_compare(node->id, my_cluster_id) == 0) return mtev_true;
  }
  return mtev_false;
}

static xmlNodePtr
mtev_cluster_to_xmlnode(mtev_cluster_t *c) {
  int i;
  char str[32];
  xmlNodePtr cluster;
  cluster = xmlNewNode(NULL, (xmlChar *)"cluster");
  xmlSetProp(cluster, (xmlChar *)"name", (xmlChar *)c->name);
  snprintf(str, sizeof(str), "%lld", c->config_seq);
  xmlSetProp(cluster, (xmlChar *)"seq", (xmlChar *)str);
  for(i=0;i<c->node_cnt;i++) {
    mtev_cluster_node_t *n = &c->nodes[i];
    xmlNodePtr node;
    char uuid_str[UUID_STR_LEN+1], port[6], ipstr[INET6_ADDRSTRLEN];
    node = xmlNewNode(NULL, (xmlChar *)"node");
    uuid_unparse_lower(n->id, uuid_str);
    xmlSetProp(node, (xmlChar *)"id", (xmlChar *)uuid_str);
    xmlSetProp(node, (xmlChar *)"cn", (xmlChar *)n->cn);
    if(n->addr.addr4.sin_family == AF_INET) {
      inet_ntop(AF_INET, &n->addr.addr4.sin_addr,
                ipstr, sizeof(ipstr));
      xmlSetProp(node, (xmlChar *)"address", (xmlChar *)ipstr);
      snprintf(port, sizeof(port), "%d", ntohs(n->addr.addr4.sin_port));
      xmlSetProp(node, (xmlChar *)"port", (xmlChar *)port);
    }
    else if(n->addr.addr6.sin6_family == AF_INET6) {
      inet_ntop(AF_INET6, &n->addr.addr6.sin6_addr,
                ipstr, sizeof(ipstr));
      xmlSetProp(node, (xmlChar *)"address", (xmlChar *)ipstr);
      snprintf(port, sizeof(port), "%d", ntohs(n->addr.addr6.sin6_port));
      xmlSetProp(node, (xmlChar *)"port", (xmlChar *)port);
    }
    xmlAddChild(cluster, node);
  }
  return cluster;
}

#define FAIL(a) do { error = (a); goto error; } while(0)

static int
rest_show_cluster(mtev_http_rest_closure_t *restc, int n, char **p) {
  mtev_http_session_ctx *ctx = restc->http_ctx;
  xmlDocPtr doc;
  xmlNodePtr root;

  doc = xmlNewDoc((xmlChar *)"1.0");
  root = xmlNewDocNode(doc, NULL, (xmlChar *)"clusters", NULL);
  xmlDocSetRootElement(doc, root);
  if(n == 1) {
    mtev_cluster_t *c = mtev_cluster_by_name(p[0]);
    if(!c) goto notfound;
    xmlAddChild(root, mtev_cluster_to_xmlnode(c));
  }
  else {
    mtev_hash_iter iter = MTEV_HASH_ITER_ZERO;
    void *vc;
    const char *key;
    int klen;
    while(mtev_hash_next(&global_clusters, &iter, &key, &klen, &vc)) {
      xmlAddChild(root, mtev_cluster_to_xmlnode((mtev_cluster_t *)vc));
    }
  }
  mtev_http_response_standard(ctx, 200, "OK", "text/xml");
  mtev_http_response_xml(ctx, doc);
  mtev_http_response_end(ctx);
  goto cleanup;

 notfound:
  mtev_http_response_standard(ctx, 404, "ERROR", "text/xml");
  mtev_http_response_end(ctx);
  goto cleanup;

 cleanup:
  if(doc) xmlFreeDoc(doc);
  return 0;
}

static int
rest_update_cluster(mtev_http_rest_closure_t *restc, int n, char **p) {
  mtev_http_session_ctx *ctx = restc->http_ctx;
  const char *error = "internal error";
  int complete = 0, mask = 0, error_code = 500, status;
  xmlDocPtr indoc, doc;
  xmlNodePtr root;

  indoc = rest_get_xml_upload(restc, &mask, &complete);
  if(!complete) return mask;
  if(indoc == NULL) FAIL("xml parse error");
  root = xmlDocGetRootElement(indoc);
  if(!root || strcmp((const char *)root->name, "cluster"))
    FAIL("bad root node: not cluster");
  status = mtev_cluster_update_internal(root, mtev_true);
  if(status < 0) {
    FAIL("failed to update");
  }
  mtev_http_response_standard(ctx, status == 2 ? 304 : 204, "OK", "none");
  mtev_http_response_end(ctx);
  return 0;

 error:
  mtev_http_response_standard(ctx, error_code, "ERROR", "text/xml");
  doc = xmlNewDoc((xmlChar *)"1.0");
  root = xmlNewDocNode(doc, NULL, (xmlChar *)"error", NULL);
  xmlDocSetRootElement(doc, root);
  xmlNodeAddContent(root, (xmlChar *)error);
  mtev_http_response_xml(ctx, doc);
  mtev_http_response_end(ctx);
  goto cleanup;

 cleanup:
  if(doc) xmlFreeDoc(doc);
  return 0;
}

void
mtev_cluster_init() {
  uuid_t my_id;
  char my_id_str[UUID_STR_LEN+1];
  int i, j, n_clusters, n_nodes;
  mtev_conf_section_t *clusters, parent;

  mtev_hash_init(&global_clusters);

  parent = mtev_conf_get_section(NULL, "//clusters");
  if(!parent) return;

  have_clusters = mtev_true;
  if(mtev_conf_get_stringbuf(parent, "@my_id", my_id_str, sizeof(my_id_str)) &&
     uuid_parse(my_id_str, my_id) == 0) {
    mtev_cluster_set_self(my_id);
  }
  clusters = mtev_conf_get_sections(NULL, "//clusters/cluster", &n_clusters);
  for(i=0;i<n_clusters;i++) {
    mtev_cluster_update_internal(clusters[i], mtev_false);
  }
  if(clusters) free(clusters);

  assert(mtev_http_rest_register_auth(
    "GET", "/", "^cluster(?:/(.+))?$", rest_show_cluster,
             mtev_http_rest_client_cert_auth
  ) == 0);
  assert(mtev_http_rest_register_auth(
    "POST", "/", "^cluster$", rest_update_cluster,
             mtev_http_rest_client_cert_auth
  ) == 0);
}

