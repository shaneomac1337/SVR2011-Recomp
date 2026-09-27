// svr2011 - ReXGlue Recompiled Project

#include "generated/default/svr2011_init.h"

#include "svr2011_app.h"

REXCVAR_DEFINE_BOOL(svr_profile, false, "SVR2011", "Enable the on-demand Tracy profiler");

REX_DEFINE_APP(svr2011, Svr2011App::Create)
