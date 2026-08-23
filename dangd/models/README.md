<!-- Copyright 2026 David Cornejo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Standard model fixtures

`ietf-netconf-acm@2018-02-14.yang` is the normative module published in RFC
8341 and is redistributed under the license text embedded in that module.
`ietf-yang-types@2013-07-15.yang` is its RFC 6991 dependency and retains its
embedded IETF Trust license notice.

`ietf-yang-library@2019-01-04.yang` is the normative RFC 8525 module;
`ietf-datastores@2018-02-14.yang` is its RFC 8342 dependency; and
`ietf-inet-types@2013-07-15.yang` is its RFC 6991 URI-type dependency. These
copies come from the IANA YANG Parameters registry and retain their embedded
IETF Trust license notices.

`ietf-netconf-monitoring@2010-10-04.yang` provides the RFC 6022 schema nodes
implemented by `dangd`, including the schema inventory and `get-schema` RPC.
It is derived from the RFC code component under the IETF Trust Simplified BSD
License.

`ietf-netconf-nmda@2019-01-07.yang` is the normative RFC 8526 NETCONF NMDA
module. Its RFC 8342 origin dependency, RFC 7952 metadata dependency, and RFC
6241/RFC 6243 NETCONF dependencies are pinned beside it. These unmodified
copies come from the YangModels IETF RFC registry and retain their embedded
IETF Trust license notices.

`dangd-reconciliation@2026-08-23.yang` is the project-owned operational model
for hardware actions whose compensation failed. It is implemented by the core,
published through RFC 8525 YANG Library, and retrievable through RFC 6022
`get-schema` like the bundled standards modules.
