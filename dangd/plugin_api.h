// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef DANGD_PLUGIN_API_H_
#define DANGD_PLUGIN_API_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DANG_PLUGIN_ABI_V1 1u

/** Declares how one supplied YANG source participates in server conformance. */
typedef enum DangYangSourceRoleV1 {
  DANG_YANG_IMPLEMENTED_V1 = 1,
  DANG_YANG_IMPORT_ONLY_V1 = 2,
  DANG_YANG_DEVIATION_V1 = 3
} DangYangSourceRoleV1;

/** Immutable YANG source descriptor returned by a plugin. */
typedef struct DangYangSourceV1 {
  const char* module_name;
  const char* revision;
  const char* source;
  size_t source_size;
  const char* source_uri;
  uint32_t role;
  const char* const* enabled_features;
  size_t enabled_feature_count;
} DangYangSourceV1;

/** Borrowed callback error strings, valid until the callback returns. */
typedef struct DangPluginErrorV1 {
  const char* message;
  const char* instance_path;
} DangPluginErrorV1;

/** Complete immutable configuration snapshots for one running transaction. */
typedef struct DangTransactionV1 {
  const char* before_xml;
  const char* proposed_xml;
  const char* changes_json;
} DangTransactionV1;

/** One schema-validated RPC or action invocation authorized by dangd. */
typedef struct DangOperationV1 {
  const char* module_name;
  const char* operation_name;
  const char* instance_path;
  const char* input_xml;
} DangOperationV1;

/** Borrowed operation output copied by dangd before the callback returns. */
typedef struct DangOperationResultV1 {
  const char* output_xml;
} DangOperationResultV1;

/** Version-one POSIX plugin function table and lifecycle contract. */
typedef struct DangPluginV1 {
  uint32_t abi_version;
  const char* plugin_name;
  void* context;
  size_t (*yang_source_count)(void* context);
  int (*yang_source_at)(void* context, size_t index,
                        DangYangSourceV1* source, DangPluginErrorV1* error);
  size_t (*dependency_count)(void* context);
  const char* (*dependency_at)(void* context, size_t index);
  int (*prepare)(void* context, const DangTransactionV1* transaction,
                 void** prepared, DangPluginErrorV1* error);
  int (*validate)(void* context, void* prepared, DangPluginErrorV1* error);
  int (*apply)(void* context, void* prepared, DangPluginErrorV1* error);
  int (*rollback)(void* context, void* prepared, DangPluginErrorV1* error);
  void (*release)(void* context, void* prepared);
  void (*destroy)(void* context);
} DangPluginV1;

/** Type of the required exported `dang_plugin_init_v1` entry point. */
typedef const DangPluginV1* (*DangPluginInitV1)(void);

#define DANG_PLUGIN_ABI_V2 2u

/** ABI v2 preserves the complete v1 prefix and adds operation dispatch. */
typedef struct DangPluginV2 {
  DangPluginV1 v1;
  int (*invoke)(void* context, const DangOperationV1* operation,
                DangOperationResultV1* result, DangPluginErrorV1* error);
} DangPluginV2;

/** Type of the optional exported `dang_plugin_init_v2` entry point. */
typedef const DangPluginV2* (*DangPluginInitV2)(void);

#define DANG_PLUGIN_ABI_V3 3u

/** Borrowed operational-state XML copied before the callback returns. */
typedef struct DangOperationalDataV1 {
  const char* data_xml;
} DangOperationalDataV1;

/** ABI v3 adds read-only operational-state publication. */
typedef struct DangPluginV3 {
  DangPluginV2 v2;
  int (*get_operational_data)(void* context, DangOperationalDataV1* result,
                              DangPluginErrorV1* error);
} DangPluginV3;

/** Type of the optional exported `dang_plugin_init_v3` entry point. */
typedef const DangPluginV3* (*DangPluginInitV3)(void);

#define DANG_PLUGIN_ABI_V4 4u

/** Generic ordering class for one retained hardware action. */
typedef enum DangHardwareActionClassV1 {
  DANG_HARDWARE_NORMAL_V1 = 0,
  DANG_HARDWARE_ACTIVATE_V1 = 1,
  DANG_HARDWARE_DEACTIVATE_V1 = 2
} DangHardwareActionClassV1;

/** Borrowed descriptor copied by dangd while the callback is active. */
typedef struct DangHardwareActionV1 {
  const char* action_id;
  const char* instance_path;
  uint32_t action_class;
  const char* const* dependencies;
  size_t dependency_count;
} DangHardwareActionV1;

/** ABI v4 exposes fine-grained reversible hardware actions. */
typedef struct DangPluginV4 {
  DangPluginV3 v3;
  size_t (*hardware_action_count)(void* context, void* prepared);
  int (*hardware_action_at)(void* context, void* prepared, size_t index,
                            DangHardwareActionV1* action,
                            DangPluginErrorV1* error);
  int (*apply_hardware_action)(void* context, void* prepared,
                               const char* action_id,
                               DangPluginErrorV1* error);
  int (*rollback_hardware_action)(void* context, void* prepared,
                                  const char* action_id,
                                  DangPluginErrorV1* error);
} DangPluginV4;

/** Type of the optional exported `dang_plugin_init_v4` entry point. */
typedef const DangPluginV4* (*DangPluginInitV4)(void);

#define DANG_PLUGIN_ABI_V5 5u

/** Operational XML with an explicit completeness assertion for returned nodes. */
typedef struct DangOperationalDataV2 {
  const char* data_xml;
  uint32_t complete;
} DangOperationalDataV2;

/** ABI v5 lets providers make omitted children decisively absent. */
typedef struct DangPluginV5 {
  DangPluginV4 v4;
  int (*get_operational_data_v2)(void* context, DangOperationalDataV2* result,
                                 DangPluginErrorV1* error);
} DangPluginV5;

/** Type of the optional exported `dang_plugin_init_v5` entry point. */
typedef const DangPluginV5* (*DangPluginInitV5)(void);

#define DANG_PLUGIN_ABI_V6 6u

/** How one requested configuration node exists after hardware application. */
typedef enum DangConfigurationDispositionV1 {
  DANG_CONFIGURATION_APPLIED_V1 = 1,
  DANG_CONFIGURATION_TRANSFORMED_V1 = 2,
  DANG_CONFIGURATION_REJECTED_V1 = 3,
  DANG_CONFIGURATION_DELAYED_V1 = 4
} DangConfigurationDispositionV1;

/** Borrowed, per-node result copied by dangd before the callback returns. */
typedef struct DangConfigurationOutcomeV1 {
  const char* instance_path;
  uint32_t disposition;
  const char* reason;
} DangConfigurationOutcomeV1;

/**
 * Actual complete configuration and its noteworthy per-node outcomes.
 *
 * `applied_xml` is the complete configuration currently accepted by the
 * backend, not the requested intent. It may equal `current_xml`. Outcome paths
 * must be unique across every plugin participating in the transaction.
 */
typedef struct DangAppliedConfigurationV1 {
  const char* applied_xml;
  const DangConfigurationOutcomeV1* outcomes;
  size_t outcome_count;
} DangAppliedConfigurationV1;

/** ABI v6 lets a backend report its actual post-apply configuration. */
typedef struct DangPluginV6 {
  DangPluginV5 v5;
  int (*reconcile_applied_configuration)(
      void* context, void* prepared, const char* current_xml,
      DangAppliedConfigurationV1* result, DangPluginErrorV1* error);
} DangPluginV6;

/** Type of the optional exported `dang_plugin_init_v6` entry point. */
typedef const DangPluginV6* (*DangPluginInitV6)(void);

#define DANG_PLUGIN_ABI_V7 7u

/**
 * ABI v7 declares exclusive ownership of non-schema resource domains.
 *
 * Domain identifiers are lowercase ASCII tokens such as `routing`. They let
 * dangd reject providers which expose different YANG modules but would mutate
 * the same underlying resource.
 */
typedef struct DangPluginV7 {
  DangPluginV6 v6;
  size_t (*resource_domain_count)(void* context);
  const char* (*resource_domain_at)(void* context, size_t index);
} DangPluginV7;

/** Type of the optional exported `dang_plugin_init_v7` entry point. */
typedef const DangPluginV7* (*DangPluginInitV7)(void);

#define DANG_PLUGIN_ABI_V8 8u

/** Borrowed modeled notification copied by dangd before the callback returns. */
typedef struct DangNotificationV1 {
  /** Registered stream, normally `NETCONF`. */
  const char* stream_name;
  /** Implemented module that declares the notification. */
  const char* module_name;
  /** Notification schema-node name within `module_name`. */
  const char* notification_name;
  /** Self-contained modeled event element, without the RFC 5277 wrapper. */
  const char* content_xml;
  /** Complete instance path for an associated notification, or empty. */
  const char* instance_path;
  /** Nonzero requests default denial in addition to schema annotations. */
  uint32_t default_deny_all;
} DangNotificationV1;

/**
 * ABI v8 lets the host drain queued plugin events without blocking.
 *
 * Return one when an event was written, zero when the queue is empty, and -1
 * with `error` populated when draining failed. Dangd bounds calls per poll and
 * validates the copied event against the compiled schema before publication.
 */
typedef struct DangPluginV8 {
  DangPluginV7 v7;
  int (*next_notification)(void* context, DangNotificationV1* notification,
                           DangPluginErrorV1* error);
} DangPluginV8;

/** Type of the optional exported `dang_plugin_init_v8` entry point. */
typedef const DangPluginV8* (*DangPluginInitV8)(void);

#define DANG_PLUGIN_ABI_V9 9u

/** Role of one stable participant in a coordinated peer transaction. */
typedef enum DangPeerTransactionRoleV1 {
  DANG_PEER_PRIMARY_V1 = 1,
  DANG_PEER_STANDBY_V1 = 2
} DangPeerTransactionRoleV1;

/**
 * One complete module-scoped candidate contributed for one peer.
 *
 * `configuration_xml` is a NETCONF `<config>` document containing every
 * configured top-level node owned by `module_name`, including none when the
 * complete module image is empty. Dangd composes non-overlapping module images
 * from every affected plugin; plugins must not include another module's data.
 */
typedef struct DangPeerCandidateV1 {
  const char* group_id;
  const char* participant_id;
  /** Nonzero when this candidate is owned by the contributing local host. */
  uint32_t local;
  uint32_t role;
  uint32_t confirmed_timeout_seconds;
  const char* module_name;
  const char* configuration_xml;
  /** Opaque plugin data echoed only to this plugin's verifier. */
  const char* verification_context_json;
} DangPeerCandidateV1;

/** Authenticated readback supplied to a plugin after peer commit application. */
typedef struct DangPeerVerificationV1 {
  const char* group_id;
  const char* participant_id;
  const char* verification_context_json;
  const char* running_reply_xml;
  const char* operational_reply_xml;
} DangPeerVerificationV1;

/**
 * ABI v9 adds transport-neutral peer planning and verification.
 *
 * Planning callbacks inspect the already prepared transaction. Returned
 * strings are borrowed and copied before the callback returns. The verifier
 * receives authenticated replies but no endpoint, credential, session, or
 * transport object; those remain exclusively owned by dangd. A verifier
 * returns one when the peer is accepted, zero for permanent rejection, or
 * minus one when correct configuration has not yet converged. Dangd retries a
 * pending verifier with fresh authenticated readback until the transaction's
 * existing deadline; it never retries permanent rejection.
 */
typedef struct DangPluginV9 {
  DangPluginV8 v8;
  size_t (*peer_candidate_count)(void* context, void* prepared);
  int (*peer_candidate_at)(void* context, void* prepared, size_t index,
                           DangPeerCandidateV1* candidate,
                           DangPluginErrorV1* error);
  int (*verify_peer)(void* context, void* prepared,
                     const DangPeerVerificationV1* verification,
                     DangPluginErrorV1* error);
} DangPluginV9;

/** Type of the optional exported `dang_plugin_init_v9` entry point. */
typedef const DangPluginV9* (*DangPluginInitV9)(void);

#ifdef __cplusplus
}
#endif

#endif  // DANGD_PLUGIN_API_H_
