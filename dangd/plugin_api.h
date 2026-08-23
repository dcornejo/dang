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

#ifdef __cplusplus
}
#endif

#endif  // DANGD_PLUGIN_API_H_
