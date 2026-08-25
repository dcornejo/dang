/* Copyright 2026 David Cornejo */
/* SPDX-License-Identifier: Apache-2.0 */

/*
 * Independent RFC 8341 oracle used by run_sysrepo_nacm_interop.sh. This file
 * intentionally uses only public sysrepo/libyang APIs and does not link dang.
 */
#include <libyang/libyang.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sysrepo.h>
#include <sysrepo/netconf_acm.h>

static int rpc_cb(sr_session_ctx_t *session, uint32_t sub_id,
                  const char *xpath, const sr_val_t *input, size_t input_cnt,
                  sr_event_t event, uint32_t request_id, sr_val_t **output,
                  size_t *output_cnt, void *private_data) {
  (void)session;
  (void)sub_id;
  (void)xpath;
  (void)input;
  (void)input_cnt;
  (void)event;
  (void)request_id;
  (void)output;
  (void)output_cnt;
  (void)private_data;
  return SR_ERR_OK;
}

static int report(const char *name, int actual, int expected) {
  const int passed = actual == expected;
  printf("%-34s %s (actual=%s expected=%s)\n", name,
         passed ? "PASS" : "FAIL", sr_strerror(actual),
         sr_strerror(expected));
  return passed ? 0 : 1;
}

static int parse_and_apply(sr_conn_ctx_t *conn, sr_session_ctx_t *session,
                           const char *xml) {
  const struct ly_ctx *context = sr_acquire_context(conn);
  struct lyd_node *edit = NULL;
  LY_ERR lyrc = lyd_parse_data_mem(context, xml, LYD_XML,
                                   LYD_PARSE_STRICT | LYD_PARSE_ONLY, 0, &edit);
  if (lyrc != LY_SUCCESS) {
    sr_release_context(conn);
    return SR_ERR_LY;
  }
  int rc = sr_edit_batch(session, edit, "merge");
  lyd_free_siblings(edit);
  sr_release_context(conn);
  if (rc == SR_ERR_OK) {
    rc = sr_apply_changes(session, 0);
  }
  return rc;
}

int main(void) {
  static const char initial_xml[] =
      "<nacm xmlns=\"urn:ietf:params:xml:ns:yang:ietf-netconf-acm\">"
      "<read-default>deny</read-default>"
      "<write-default>deny</write-default>"
      "<exec-default>deny</exec-default>"
      "<enable-external-groups>false</enable-external-groups>"
      "<groups><group><name>operators</name><user-name>alice</user-name>"
      "</group></groups>"
      "<rule-list><name>operator-rules</name><group>operators</group>"
      "<rule><name>hostname</name><module-name>dang-nacm-interop</module-name>"
      "<path xmlns:dni=\"urn:dang:nacm:interop\">"
      "/dni:system/dni:hostname</path>"
      "<access-operations>read update</access-operations>"
      "<action>permit</action></rule>"
      "<rule><name>reboot</name><module-name>dang-nacm-interop</module-name>"
      "<rpc-name>reboot</rpc-name><access-operations>exec</access-operations>"
      "<action>permit</action></rule>"
      "</rule-list></nacm>"
      "<system xmlns=\"urn:dang:nacm:interop\">"
      "<hostname>router-a</hostname><secret>not-for-alice</secret></system>";
  sr_conn_ctx_t *conn = NULL;
  sr_session_ctx_t *admin = NULL;
  sr_session_ctx_t *alice = NULL;
  sr_session_ctx_t *bob = NULL;
  sr_subscription_ctx_t *nacm_sub = NULL;
  sr_subscription_ctx_t *rpc_sub = NULL;
  sr_data_t *data = NULL;
  sr_val_t *output = NULL;
  size_t output_count = 0;
  char *printed = NULL;
  int failures = 0;
  int rc;

  if ((rc = sr_connect(0, &conn)) != SR_ERR_OK ||
      (rc = sr_session_start(conn, SR_DS_RUNNING, &admin)) != SR_ERR_OK ||
      (rc = sr_nacm_init(admin, 0, &nacm_sub)) != SR_ERR_OK ||
      (rc = parse_and_apply(conn, admin, initial_xml)) != SR_ERR_OK ||
      (rc = sr_rpc_subscribe(admin, "/dang-nacm-interop:reboot", rpc_cb,
                             NULL, 0, 0, &rpc_sub)) != SR_ERR_OK ||
      (rc = sr_session_start(conn, SR_DS_RUNNING, &alice)) != SR_ERR_OK ||
      (rc = sr_nacm_set_user(alice, "alice")) != SR_ERR_OK ||
      (rc = sr_session_start(conn, SR_DS_RUNNING, &bob)) != SR_ERR_OK ||
      (rc = sr_nacm_set_user(bob, "bob")) != SR_ERR_OK) {
    fprintf(stderr, "setup failed: %s\n", sr_strerror(rc));
    failures = 1;
    goto cleanup;
  }

  rc = sr_get_data(alice, "/dang-nacm-interop:*", 0, 0, 0, &data);
  failures += report("alice readable data request", rc, SR_ERR_OK);
  if (rc == SR_ERR_OK && data != NULL) {
    if (lyd_print_mem(&printed, data->tree, LYD_XML,
                      LYD_PRINT_WITHSIBLINGS) != LY_SUCCESS ||
        strstr(printed, "<hostname>router-a</hostname>") == NULL ||
        strstr(printed, "secret") != NULL) {
      puts("alice read filtering               FAIL");
      ++failures;
    } else {
      puts("alice read filtering               PASS");
    }
    free(printed);
    printed = NULL;
    sr_release_data(data);
    data = NULL;
  }

  rc = sr_set_item_str(alice,
      "/dang-nacm-interop:system/hostname", "router-b", NULL, 0);
  if (rc == SR_ERR_OK) {
    rc = sr_apply_changes(alice, 0);
  }
  failures += report("alice hostname update", rc, SR_ERR_OK);

  rc = sr_set_item_str(alice,
      "/dang-nacm-interop:system/secret", "stolen", NULL, 0);
  if (rc == SR_ERR_OK) {
    rc = sr_apply_changes(alice, 0);
  }
  failures += report("alice secret update", rc, SR_ERR_UNAUTHORIZED);
  sr_discard_changes(alice);

  rc = sr_rpc_send(alice, "/dang-nacm-interop:reboot", NULL, 0, 0,
                   &output, &output_count);
  failures += report("alice reboot execution", rc, SR_ERR_OK);
  free(output);
  output = NULL;

  rc = sr_get_data(bob, "/dang-nacm-interop:*", 0, 0, 0, &data);
  failures += report("bob readable data request", rc, SR_ERR_OK);
  if (rc == SR_ERR_OK && data == NULL) {
    puts("bob read filtering                 PASS");
  } else {
    puts("bob read filtering                 FAIL");
    ++failures;
  }
  if (data != NULL) {
    sr_release_data(data);
    data = NULL;
  }

  rc = sr_rpc_send(bob, "/dang-nacm-interop:reboot", NULL, 0, 0,
                   &output, &output_count);
  failures += report("bob reboot execution", rc, SR_ERR_UNAUTHORIZED);

cleanup:
  if (bob != NULL) {
    sr_session_stop(bob);
  }
  if (alice != NULL) {
    sr_session_stop(alice);
  }
  if (rpc_sub != NULL) {
    sr_unsubscribe(rpc_sub);
  }
  if (admin != NULL) {
    /* Remove cross-module NACM paths before the shell uninstalls this model. */
    int cleanup_rc = sr_delete_item(admin, "/ietf-netconf-acm:nacm", 0);
    if (cleanup_rc == SR_ERR_OK) {
      cleanup_rc = sr_delete_item(admin, "/dang-nacm-interop:system", 0);
    }
    if (cleanup_rc == SR_ERR_OK) {
      cleanup_rc = sr_apply_changes(admin, 0);
    }
    if (cleanup_rc != SR_ERR_OK) {
      fprintf(stderr, "datastore cleanup failed: %s\n",
              sr_strerror(cleanup_rc));
      failures = 1;
    }
  }
  if (nacm_sub != NULL) {
    sr_unsubscribe(nacm_sub);
  }
  if (admin != NULL) {
    sr_session_stop(admin);
  }
  if (conn != NULL) {
    sr_nacm_destroy();
    sr_disconnect(conn);
  }
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
