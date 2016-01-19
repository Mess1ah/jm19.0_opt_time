
/*!
 ***********************************************************************
 *  \file
 *     configfile.h
 *  \brief
 *     Prototypes for configfile.c and definitions of used structures.
 ***********************************************************************
 */

#ifndef _CONFIGFILE_H_
#define _CONFIGFILE_H_

#define DEFAULTCONFIGFILENAME "decoder.cfg"

#include "config_common.h"
//#define PROFILE_IDC     88
//#define LEVEL_IDC       21


InputParameters cfgparams;

#ifdef INCLUDED_BY_CONFIGFILE_C
// Mapping_Map Syntax:
// {NAMEinConfigFile,  &cfgparams.VariableName, Type, InitialValue, LimitType, MinLimit, MaxLimit, CharSize}
// Types : {0:int, 1:text, 2: double}
// LimitType: {0:none, 1:both, 2:minimum, 3: QP based}
// We could separate this based on types to make it more flexible and allow also defaults for text types.
Mapping Map[] = {
    {"InputFile",                &cfgparams.infile,                       1,   0.0,                       0,  0.0,              0.0,             FILE_NAME_SIZE, },
		{"KeyFileDir", 							 &cfgparams.keyfile_dir, 									1,	 0.0, 											0,	0.0,							0.0,						 FILE_NAME_SIZE, },			
		{"EnableKey",                &cfgparams.enable_key,                   0,   1.0,                       1,  0.0,              1.0,                             },			
    {"FileFormat",               &cfgparams.FileFormat,                   0,   0.0,                       1,  0.0,              1.0,                             },
    {"RefOffset",                &cfgparams.ref_offset,                   0,   0.0,                       1,  0.0,              256.0,                             },
    {"POCScale",                 &cfgparams.poc_scale,                    0,   2.0,                       1,  1.0,              10.0,                            },
    {"DisplayDecParams",         &cfgparams.bDisplayDecParams,            0,   1.0,                       1,  0.0,              1.0,                             },
    {"Silent",                   &cfgparams.silent,                       0,   0.0,                       1,  0.0,              1.0,                             },
    {"IntraProfileDeblocking",   &cfgparams.intra_profile_deblocking,     0,   1.0,                       1,  0.0,              1.0,                             },
    {"DecFrmNum",                &cfgparams.iDecFrmNum,                   0,   0.0,                       2,  0.0,              0.0,                             },
#if (MVC_EXTENSION_ENABLE)
    {"DecodeAllLayers",          &cfgparams.DecodeAllLayers,              0,   0.0,                       1,  0.0,              1.0,                             },
#endif
    {"DPBPLUS0",                 &cfgparams.dpb_plus[0],                  0,   1.0,                       1,  -16.0,            16.0,                             },
    {"DPBPLUS1",                 &cfgparams.dpb_plus[1],                  0,   0.0,                       1,  -16.0,            16.0,                             },
    {NULL,                       NULL,                                   -1,   0.0,                       0,  0.0,              0.0,                             },
};
#endif

#ifndef INCLUDED_BY_CONFIGFILE_C
extern Mapping Map[];
#endif
extern void JMDecHelpExit ();
extern void ParseCommand(InputParameters *p_Inp, int ac, char *av[]);

#endif

