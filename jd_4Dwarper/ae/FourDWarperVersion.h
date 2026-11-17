// 4Dwarper version, shared by the C++ code and the PiPL resource. Bump together.
// W4D_PIPL_VERSION must equal PF_VERSION(MAJOR, MINOR, BUG, STAGE, BUILD); FourDWarper.cpp checks it at compile time.
#pragma once

#define W4D_MAJOR_VERSION  0
#define W4D_MINOR_VERSION  3
#define W4D_BUG_VERSION    5
#define W4D_STAGE_VERSION  0   /* PF_Stage_DEVELOP */
#define W4D_BUILD_VERSION  1

/* (0<<19) | (3<<15) | (5<<11) | (0<<9) | 1 */
#define W4D_PIPL_VERSION   108545

#define W4D_NAME           "jd_4Dwarper 0.3.5"   /* menu / display name with the version, like the other JD tools
                                                 (0.3.5; bump it with the version). PiPLtool wants one literal */
#define W4D_CATEGORY       "JD tools"
#define W4D_MATCH_NAME     "JDEL 4Dwarper"   /* NEVER change (kept in 0.3.4 on Jean's word): projects find the effect by it */
#define W4D_SUPPORT_URL    "https://jeandelaunay.fr"
#define W4D_DOCS_URL       "https://github.com/jdel0nAY/ae-tools/tree/main/jd_4Dwarper"   /* Open Docs button (ae-tools from the next build) */
