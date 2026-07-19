#include "stdafx.h"
#include "component_version.h"

DECLARE_COMPONENT_VERSION(
    MULTIROOM_PRODUCT_NAME,
    MULTIROOM_COMPONENT_VERSION,
    "Native AirPlay 2 multiroom transport component for foobar2000. "
    "This build contains AirPlay 2 discovery classification, a foobar UI speaker selector, "
    "a transport-neutral core, the sync scheduler, and transient encrypted lossless AirPlay 2 streaming. "
    "PIN pairing and persisted AirPlay credentials are available from Preferences.\n\n"
    "Repository: https://github.com/ArtifexEt/foobar-universal-multiroom");

VALIDATE_COMPONENT_FILENAME("foo_out_multiroom_bridge.dll");
FOOBAR2000_IMPLEMENT_CFG_VAR_DOWNGRADE;
