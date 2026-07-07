#include "stdafx.h"
#include "component_version.h"

DECLARE_COMPONENT_VERSION(
    "Universal Multiroom Bridge",
    MULTIROOM_COMPONENT_VERSION,
    "Native multiroom transport component for foobar2000. "
    "This build contains native AirPlay discovery, a foobar UI speaker selector, "
    "the transport-neutral core, and the sync scheduler.\n\n"
    "Repository: https://github.com/ArtifexEt/foobar-universal-multiroom");

VALIDATE_COMPONENT_FILENAME("foo_out_multiroom_bridge.dll");
FOOBAR2000_IMPLEMENT_CFG_VAR_DOWNGRADE;
