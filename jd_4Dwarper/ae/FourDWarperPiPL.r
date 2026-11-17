#include "AEConfig.h"
#include "AE_EffectVers.h"
#include "FourDWarperVersion.h"

#ifndef AE_OS_WIN
	#include "AE_General.r"
#endif

resource 'PiPL' (16000) {
	{
		Kind { AEEffect },
		Name { W4D_NAME },
		Category { W4D_CATEGORY },
#ifdef AE_OS_WIN
    #if defined(AE_PROC_INTELx64)
		CodeWin64X86 {"EffectMain"},
    #elif defined(AE_PROC_ARM64)
		CodeWinARM64 {"EffectMain"},
    #endif
#endif
		AE_PiPL_Version { 2, 0 },
		AE_Effect_Spec_Version { PF_PLUG_IN_VERSION, PF_PLUG_IN_SUBVERS },
		AE_Effect_Version { W4D_PIPL_VERSION },
		AE_Effect_Info_Flags { 0 },
		/* must match GlobalSetup: WIDE_TIME_INPUT | CUSTOM_UI | DEEP_COLOR_AWARE */
		AE_Effect_Global_OutFlags { 0x02008002 },
		/* must match GlobalSetup: SUPPORTS_SMART_RENDER | FLOAT_COLOR_AWARE | SUPPORTS_THREADED_RENDERING */
		AE_Effect_Global_OutFlags_2 { 0x08001400 },
		AE_Effect_Match_Name { W4D_MATCH_NAME },
		AE_Reserved_Info { 8 },
		AE_Effect_Support_URL { W4D_SUPPORT_URL }
	}
};
