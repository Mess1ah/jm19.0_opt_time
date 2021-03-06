/*!
 ***********************************************************************
 * \file macroblock.c
 *
 * \brief
 *     Decode a Macroblock
 *
 * \author
 *    Main contributors (see contributors.h for copyright, address and affiliation details)
 *    - Inge Lille-Lang鴜               <inge.lille-langoy@telenor.com>
 *    - Rickard Sjoberg                 <rickard.sjoberg@era.ericsson.se>
 *    - Jani Lainema                    <jani.lainema@nokia.com>
 *    - Sebastian Purreiter             <sebastian.purreiter@mch.siemens.de>
 *    - Thomas Wedi                     <wedi@tnt.uni-hannover.de>
 *    - Detlev Marpe
 *    - Gabi Blaettermann
 *    - Ye-Kui Wang                     <wyk@ieee.org>
 *    - Lowell Winger                   <lwinger@lsil.com>
 *    - Alexis Michael Tourapis         <alexismt@ieee.org>
 ***********************************************************************
*/

#include "contributors.h"

#include <math.h>

#include "global.h"
#include "mbuffer.h"
#include "elements.h"
//#include "errorconcealment.h"
#include "macroblock.h"
#include "fmo.h"
#include "cabac.h"
#include "vlc.h"
#include "image.h"
#include "mb_access.h"
#include "biaridecod.h"
#include "fast_memory.h"
#include "filehandle.h"


#if TRACE
#define TRACE_STRING(s) strncpy(currSE.tracestring, s, TRACESTRING_SIZE)
#define TRACE_DECBITS(i) dectracebitcnt(1)
#define TRACE_PRINTF(s) sprintf(type, "%s", s);
#define TRACE_STRING_P(s) strncpy(currSE->tracestring, s, TRACESTRING_SIZE)
#else
#define TRACE_STRING(s)
#define TRACE_DECBITS(i)
#define TRACE_PRINTF(s) 
#define TRACE_STRING_P(s)
#endif

//! look up tables for FRExt_chroma support
void dectracebitcnt(int count);

extern int s_cur_mvd_bitpos;

extern void setup_read_macroblock              (Slice *currSlice);
extern void set_read_CBP_and_coeffs_cabac      (Slice *currSlice);
extern void set_read_CBP_and_coeffs_cavlc      (Slice *currSlice);
extern void read_coeff_4x4_CAVLC               (Macroblock *currMB, int block_type, int i, int j, int levarr[16], int runarr[16], int *number_coefficients);
extern void read_coeff_4x4_CAVLC_444           (Macroblock *currMB, int block_type, int i, int j, int levarr[16], int runarr[16], int *number_coefficients);

static void read_motion_info_from_NAL_p_slice  (Macroblock *currMB);
static void read_motion_info_from_NAL_b_slice  (Macroblock *currMB);

extern void update_direct_types                (Slice *currSlice);
extern void set_intra_prediction_modes         (Slice *currSlice);
extern void set_read_comp_coeff_cavlc          (Macroblock *currMB);
extern void set_read_comp_coeff_cabac          (Macroblock *currMB);

/*!
 ************************************************************************
 * \brief
 *    Set context for reference frames
 ************************************************************************
 */
static inline int BType2CtxRef (int btype)
{
  return (btype >= 4);
}

/*!
 ************************************************************************
 * \brief
 *    Function for reading the reference picture indices using VLC
 ************************************************************************
 */
static char readRefPictureIdx_VLC(Macroblock *currMB, SyntaxElement *currSE, DataPartition *dP, char b8mode, int list)
{
#if TRACE
  trace_info(currSE, "ref_idx_l", list);
#endif
  currSE->context = BType2CtxRef (b8mode);
  currSE->value2 = list;
  dP->readSyntaxElement (currMB, currSE, dP);
  return (char) currSE->value1;
}

/*!
 ************************************************************************
 * \brief
 *    Function for reading the reference picture indices using FLC
 ************************************************************************
 */
static char readRefPictureIdx_FLC(Macroblock *currMB, SyntaxElement *currSE, DataPartition *dP, char b8mode, int list)
{
#if TRACE
  trace_info(currSE, "ref_idx_l", list);
#endif

  currSE->context = BType2CtxRef (b8mode);
  currSE->len = 1;
  readSyntaxElement_FLC(currSE, dP->bitstream);
  currSE->value1 = 1 - currSE->value1;

  return (char) currSE->value1;
}

/*!
 ************************************************************************
 * \brief
 *    Dummy Function for reading the reference picture indices
 ************************************************************************
 */
static char readRefPictureIdx_Null(Macroblock *currMB, SyntaxElement *currSE, DataPartition *dP, char b8mode, int list)
{
  return 0;
}

/*!
 ************************************************************************
 * \brief
 *    Function to prepare reference picture indice function pointer
 ************************************************************************
 */
static void prepareListforRefIdx ( Macroblock *currMB, SyntaxElement *currSE, DataPartition *dP, int num_ref_idx_active, int refidx_present)
{  
  if(num_ref_idx_active > 1)
  {
    if (currMB->p_Vid->active_pps->entropy_coding_mode_flag == (Boolean) CAVLC || dP->bitstream->ei_flag)
    {
      currSE->mapping = linfo_ue;
      if (refidx_present)
        currMB->readRefPictureIdx = (num_ref_idx_active == 2) ? readRefPictureIdx_FLC : readRefPictureIdx_VLC;
      else
        currMB->readRefPictureIdx = readRefPictureIdx_Null;
    }
    else
    {
      currSE->reading = readRefFrame_CABAC;
      currMB->readRefPictureIdx = (refidx_present) ? readRefPictureIdx_VLC : readRefPictureIdx_Null;
    }
  }
  else
    currMB->readRefPictureIdx = readRefPictureIdx_Null; 
}

/*!
************************************************************************
* \brief
*    updates chroma QP according to luma QP and bit depth
************************************************************************
*/
void update_qp(Macroblock *currMB, int qp)
{
  set_read_comp_coeff_cavlc(currMB);
  set_read_comp_coeff_cabac(currMB);
}

void read_delta_quant(SyntaxElement *currSE, DataPartition *dP, Macroblock *currMB, const byte *partMap, int type)
{
  Slice *currSlice = currMB->p_Slice;
  VideoParameters *p_Vid = currMB->p_Vid;
 
  currSE->type = type;

  dP = &(currSlice->partArr[partMap[currSE->type]]);

  if (p_Vid->active_pps->entropy_coding_mode_flag == (Boolean) CAVLC || dP->bitstream->ei_flag)
  {
    currSE->mapping = linfo_se;
  }
  else
    currSE->reading= read_dQuant_CABAC;

  TRACE_STRING_P("mb_qp_delta");

  dP->readSyntaxElement(currMB, currSE, dP);
  currMB->delta_quant = (short) currSE->value1;
  if ((currMB->delta_quant < -(26 + p_Vid->bitdepth_luma_qp_scale/2)) || (currMB->delta_quant > (25 + p_Vid->bitdepth_luma_qp_scale/2)))
  {
      printf("mb_qp_delta is out of range (%d)\n", currMB->delta_quant);
    currMB->delta_quant = iClip3(-(26 + p_Vid->bitdepth_luma_qp_scale/2), (25 + p_Vid->bitdepth_luma_qp_scale/2), currMB->delta_quant);

    //error ("mb_qp_delta is out of range", 500);
  }

  currSlice->qp = ((currSlice->qp + currMB->delta_quant + 52 + 2*p_Vid->bitdepth_luma_qp_scale)%(52+p_Vid->bitdepth_luma_qp_scale)) - p_Vid->bitdepth_luma_qp_scale;
  update_qp(currMB, currSlice->qp);
}

/*!
 ************************************************************************
 * \brief
 *    Function to read reference picture indice values
 ************************************************************************
 */
static void readMBRefPictureIdx (SyntaxElement *currSE, DataPartition *dP, Macroblock *currMB, PicMotionParams **mv_info, int list, int step_v0, int step_h0)
{
  if (currMB->mb_type == 1)	//P16x16
  {
    if ((currMB->b8pdir[0] == list || currMB->b8pdir[0] == BI_PRED))
    {
      int j, i;
      char refframe;
      

      currMB->subblock_x = 0;
      currMB->subblock_y = 0;
      refframe = currMB->readRefPictureIdx(currMB, currSE, dP, 1, list);	//readRefPictureIdx_FLC readRefPictureIdx_VLC
			#if	0      
      for (j = 0; j <  step_v0; ++j)
      {
        char *ref_idx = &mv_info[j][currMB->block_x].ref_idx[list];
        // for (i = currMB->block_x; i < currMB->block_x + step_h0; ++i)
        for (i = 0; i < step_h0; ++i)
        {
          //mv_info[j][i].ref_idx[list] = refframe;
          *ref_idx = refframe;
          ref_idx += sizeof(PicMotionParams);
        }
      }
			#endif
    }
  }
  else if (currMB->mb_type == 2)	//P16x8
  {
    int k, j, i, j0;
    char refframe;

    for (j0 = 0; j0 < 4; j0 += step_v0)
    {
      k = j0;

      if ((currMB->b8pdir[k] == list || currMB->b8pdir[k] == BI_PRED))
      {
        currMB->subblock_y = j0 << 2;
        currMB->subblock_x = 0;
        refframe = currMB->readRefPictureIdx(currMB, currSE, dP, currMB->b8mode[k], list);
				#if 0
        for (j = j0; j < j0 + step_v0; ++j)
        {
          char *ref_idx = &mv_info[j][currMB->block_x].ref_idx[list];
          // for (i = currMB->block_x; i < currMB->block_x + step_h0; ++i)
          for (i = 0; i < step_h0; ++i)
          {
            //mv_info[j][i].ref_idx[list] = refframe;
            *ref_idx = refframe;
            ref_idx += sizeof(PicMotionParams);
          }
        }
				#endif
      }
    }
  }  
  else if (currMB->mb_type == 3)	//P8x16
  {
    int k, j, i, i0;
    char refframe;

    currMB->subblock_y = 0;
    for (i0 = 0; i0 < 4; i0 += step_h0)
    {      
      k = (i0 >> 1);

      if ((currMB->b8pdir[k] == list || currMB->b8pdir[k] == BI_PRED) && currMB->b8mode[k] != 0)
      {
        currMB->subblock_x = i0 << 2;
        refframe = currMB->readRefPictureIdx(currMB, currSE, dP, currMB->b8mode[k], list);
				#if 0
        for (j = 0; j < step_v0; ++j)
        {
          char *ref_idx = &mv_info[j][currMB->block_x + i0].ref_idx[list];
          // for (i = currMB->block_x; i < currMB->block_x + step_h0; ++i)
          for (i = 0; i < step_h0; ++i)
          {
            //mv_info[j][i].ref_idx[list] = refframe;
            *ref_idx = refframe;
            ref_idx += sizeof(PicMotionParams);
          }
        }
				#endif
      }
    }
  }
  else
  {
    int k, j, i, j0, i0;
    char refframe;

    for (j0 = 0; j0 < 4; j0 += step_v0)
    {
      currMB->subblock_y = j0 << 2;
      for (i0 = 0; i0 < 4; i0 += step_h0)
      {      
        k = 2 * (j0 >> 1) + (i0 >> 1);

        if ((currMB->b8pdir[k] == list || currMB->b8pdir[k] == BI_PRED) && currMB->b8mode[k] != 0)
        {
          currMB->subblock_x = i0 << 2;
          refframe = currMB->readRefPictureIdx(currMB, currSE, dP, currMB->b8mode[k], list);
					#if 0
          for (j = j0; j < j0 + step_v0; ++j)
          {
            char *ref_idx = &mv_info[j][currMB->block_x + i0].ref_idx[list];
            //PicMotionParams *mvinfo = mv_info[j] + currMB->block_x + i0;
            for (i = 0; i < step_h0; ++i)
            {
              //(mvinfo++)->ref_idx[list] = refframe;
              *ref_idx = refframe;
              ref_idx += sizeof(PicMotionParams);
            }
          }
					#endif
        }
      }
    }
  }
}

/*!
************************************************************************
* \brief
*    Function to read reference picture indice values
************************************************************************
*/
//将bitoffset变成 byteoffset*8 + bitoffset
void analysis_bitoffset(int* byteoffset, int* bitoffset)
{
	int i = 0;
	int org = *bitoffset;

	while(org>7)
	{
		org -= 8;
		i ++;
	}

	*bitoffset = org;
	*byteoffset = i;
}
	
//extern int Generate_Key(int LastByteOffset,int ByteOffset,int BitOffset,int BitLength,FILE* KeyFile,int h264fd);

//extern int Generate_Key(int LastByteOffset,int ByteOffset,int BitOffset,int BitLength,FILE* KeyFile,int h264fd);
extern KeyUnit* g_pKeyUnitBuffer;
extern int g_KeyUnitIdx;
extern int g_KeyUnitBufferSize;

//RBSP_offset:从RBSP(NALU=header+RBSP)开始的位偏移
void write_mvd2keyfile(int bit_offset_from_rbsp, int KeyDataLen, int mvd, int mvd_num)
{
	if(p_Dec->p_Inp->enable_key)
	{
		FILE* p_KeyFile = p_Dec->p_KeyFile;
		int ByteOffset = 0; 	
		int BitOffset = bit_offset_from_rbsp;
		int cur_rbsp_absolute_pos = p_Dec->nalu_pos_array[p_Dec->nalu_pos_array_idx] + 1;

		analysis_bitoffset(&ByteOffset,&BitOffset);
		int mvd_absolute_byte_pos = cur_rbsp_absolute_pos + ByteOffset;	//当前RBSP位置+字节偏移,定位到MVD所在字节处(绝对偏移)

		int diff = mvd_absolute_byte_pos - p_Dec->pre_mvd_absolute_byte_pos;
		p_Dec->pre_mvd_absolute_byte_pos = mvd_absolute_byte_pos; 
		
		if(diff < 0 || BitOffset < 0)
		{
			printf("diff: %d, BitOffset: %d\n",diff,BitOffset);
			error_KeyGen("[Byte offset diff] or [BitOffset] less-than 0, they should not less-than 0!",1);
		}	

		//put the key datas into the key unit buffer
		if(g_KeyUnitIdx >= g_KeyUnitBufferSize - 1)
		{
			//printf("\033[1;31m tmp_test===============idx: %d======= \033[0m \n",g_KeyUnitIdx);
			g_KeyUnitBufferSize += KEY_UNIT_BUFFER_SIZE_APPEND;
			g_pKeyUnitBuffer = (KeyUnit*)realloc(g_pKeyUnitBuffer, g_KeyUnitBufferSize);			
		}
		g_pKeyUnitBuffer[g_KeyUnitIdx].byte_offset 		= diff;
		g_pKeyUnitBuffer[g_KeyUnitIdx].bit_offset 		= BitOffset;
		g_pKeyUnitBuffer[g_KeyUnitIdx].key_data_len 	= KeyDataLen;
		g_KeyUnitIdx ++;		
#if 0
#if H264_KEY_CREATE		
		//Generate_Key(pre_MVD_BOffset,mvd_absolute_byte_pos,BitOffset,KeyDataLen,p_KeyFile,p_Dec->BitStreamFile);
#else
		char s[400];
		//cur_rbsp_pos + ByteOffset = 读取MVD的第一个字节
		snprintf(s,400,"RBSP_start: %4d, bit_offset_from_rbsp: %4d, ByteOffset: %4d, mvd_absolute_byte_pos: %4d, BitOffset: %3d, KeyDataLen: %3d, mvd_num: %2d, mvd_sum: %3d\n",
						cur_rbsp_absolute_pos,bit_offset_from_rbsp,ByteOffset,mvd_absolute_byte_pos,BitOffset,KeyDataLen,mvd_num,mvd); 	
		fwrite(s,strlen(s),1,p_KeyFile);	
#endif
#endif
	}
}
 
static void readMBMotionVectors (SyntaxElement *currSE, DataPartition *dP, Macroblock *currMB, int list, int step_h0, int step_v0)
{
  if (currMB->mb_type == P16x16)
  {
    if ((currMB->b8pdir[0] == list || currMB->b8pdir[0]== BI_PRED))//has forward vector
    {
      int i4, j4, ii, jj;
      short curr_mvd[2];
      MotionVector pred_mv, curr_mv;
      short (*mvd)[4][2];
      //VideoParameters *p_Vid = currMB->p_Vid;
      PicMotionParams **mv_info = currMB->p_Slice->dec_picture->mv_info;
      PixelPos block[4]; // neighbor blocks
			int key_data_len = 0;
			int first_sy_len = 0;
			int bit_offset_from_rbsp = 0;		//bit offset from the current RBSP			

      currMB->subblock_x = 0; // position used for context determination
      currMB->subblock_y = 0; // position used for context determination
      i4  = currMB->block_x;
      j4  = currMB->block_y;
      mvd = &currMB->mvd [list][0];

      //get_neighbors(currMB, block, 0, 0, step_h0 << 2);

      // first get MV predictor
      //currMB->GetMVPredictor (currMB, block, &pred_mv, mv_info[j4][i4].ref_idx[list], mv_info, list, 0, 0, step_h0 << 2, step_v0 << 2);

      // X component
#if TRACE
      trace_info(currSE, "mvd0_l", list);
#endif
			bit_offset_from_rbsp = s_cur_mvd_bitpos;	//CABAC mvd bit offset

      currSE->value2 = list; // identifies the component; only used for context determination
      dP->readSyntaxElement(currMB, currSE, dP);
      curr_mvd[0] = (short) currSE->value1; 
									
			if(currMB->p_Slice->p_Vid->active_pps->entropy_coding_mode_flag == (Boolean) CAVLC)
			{
				bit_offset_from_rbsp = dP->bitstream->frame_bitoffset - currSE->len;
			}
			key_data_len += currSE->len;
			//first_sy_len = currSE->len;
			
			//write_mvd2keyfile(offset_from_rbsp-currSE->len, currSE->len,curr_mvd[0],1);

      // Y component
#if TRACE
      trace_info(currSE, "mvd1_l", list);
#endif
      currSE->value2 += 2; // identifies the component; only used for context determination
      dP->readSyntaxElement(currMB, currSE, dP);
      curr_mvd[1] = (short) currSE->value1;              

#if 0
			if(currMB->p_Slice->p_Vid->active_pps->entropy_coding_mode_flag == (Boolean) CABAC)
				offset_from_rbsp = dP->bitstream->read_len;
			else
				offset_from_rbsp = dP->bitstream->frame_bitoffset;
#endif			
			key_data_len += currSE->len;
			write_mvd2keyfile(bit_offset_from_rbsp, key_data_len,curr_mvd[0]+curr_mvd[1],2);

#if 0
      curr_mv.mv_x = (short)(curr_mvd[0] + pred_mv.mv_x);  // compute motion vector x
      curr_mv.mv_y = (short)(curr_mvd[1] + pred_mv.mv_y);  // compute motion vector y

      for(jj = j4; jj < j4 + step_v0; ++jj)
      {
        PicMotionParams *mvinfo = mv_info[jj] + i4;
        for(ii = i4; ii < i4 + step_h0; ++ii)
        {
          (mvinfo++)->mv[list] = curr_mv;
        }            
      }

      // Init first line (mvd)
      for(ii = 0; ii < step_h0; ++ii)
      {
        //*((int *) &mvd[0][ii][0]) = *((int *) curr_mvd);
        mvd[0][ii][0] = curr_mvd[0];
        mvd[0][ii][1] = curr_mvd[1];
      }              

      // now copy all other lines
      for(jj = 1; jj < step_v0; ++jj)
      {
        memcpy(mvd[jj][0], mvd[0][0],  2 * step_h0 * sizeof(short));
      }
#endif			
    }
  }
  else
  {
    int i4, j4, ii, jj;
    short curr_mvd[2];
    MotionVector pred_mv, curr_mv;
    short (*mvd)[4][2];
    //VideoParameters *p_Vid = currMB->p_Vid;
    PicMotionParams **mv_info = currMB->p_Slice->dec_picture->mv_info;
    PixelPos block[4]; // neighbor blocks

		int bit_offset_from_rbsp = 0;
		int mvd_sum = 0;
		int key_data_len = 0;
		int mvd_num = 0;

    int i, j, i0, j0, kk, k;

		/*
			--------> step_h0 = 4 horizontal skip 4 pix
			|				 _____________
			|       |							|
			|   		|							|
			|				|-------------
			^				|							|
					 		|_____________|
		stepv0 = 2 vertical skip 2 pix
		this is a : 16x8 mb type
		*/
		
    for (j0=0; j0<4; j0+=step_v0)		//vertical i0,j0 represent a mb partition
    {
      for (i0=0; i0<4; i0+=step_h0)		//horizontal
      {       
        kk = 2 * (j0 >> 1) + (i0 >> 1);

        if ((currMB->b8pdir[kk] == list || currMB->b8pdir[kk]== BI_PRED) && (currMB->b8mode[kk] != 0))//has forward vector
        {
          char cur_ref_idx = mv_info[currMB->block_y+j0][currMB->block_x+i0].ref_idx[list];
          int mv_mode  = currMB->b8mode[kk];
          int step_h = BLOCK_STEP [mv_mode][0];
          int step_v = BLOCK_STEP [mv_mode][1];
          int step_h4 = step_h << 2;
          int step_v4 = step_v << 2;
					int cur_mvd_pos = 0;

          for (j = j0; j < j0 + step_v0; j += step_v)		//vertiacl i,j represent a submb partition
          {
            currMB->subblock_y = j << 2; // position used for context determination
            j4  = currMB->block_y + j;
            mvd = &currMB->mvd [list][j];

            for (i = i0; i < i0 + step_h0; i += step_h)		
            {
              currMB->subblock_x = i << 2; // position used for context determination
              i4 = currMB->block_x + i;

              //get_neighbors(currMB, block, BLOCK_SIZE * i, BLOCK_SIZE * j, step_h4);

              // first get MV predictor
              //currMB->GetMVPredictor (currMB, block, &pred_mv, cur_ref_idx, mv_info, list, BLOCK_SIZE * i, BLOCK_SIZE * j, step_h4, step_v4);

              for (k=0; k < 2; ++k)
              {
#if TRACE
                trace_info(currSE, "mvd_l", list);
#endif
								cur_mvd_pos = s_cur_mvd_bitpos;
                currSE->value2   = (k << 1) + list; // identifies the component; only used for context determination
                dP->readSyntaxElement(currMB, currSE, dP);		//readSyntaxElement_CABAC readSyntaxElement_UVLC
                curr_mvd[k] = (short) currSE->value1; 

								if(!mvd_num)
								{
									if(currMB->p_Slice->p_Vid->active_pps->entropy_coding_mode_flag == (Boolean) CABAC)
									{
										bit_offset_from_rbsp = cur_mvd_pos;
									}
									else
									{
										bit_offset_from_rbsp = dP->bitstream->frame_bitoffset - currSE->len;
									}
								}

								mvd_num ++;
								mvd_sum += curr_mvd[k];
								key_data_len += currSE->len;								
              }
#if 0
              curr_mv.mv_x = (short)(curr_mvd[0] + pred_mv.mv_x);  // compute motion vector 
              curr_mv.mv_y = (short)(curr_mvd[1] + pred_mv.mv_y);  // compute motion vector 

              for(jj = j4; jj < j4 + step_v; ++jj)
              {
                PicMotionParams *mvinfo = mv_info[jj] + i4;
                for(ii = i4; ii < i4 + step_h; ++ii)
                {
                  (mvinfo++)->mv[list] = curr_mv;
                }            
              }

              // Init first line (mvd)
              for(ii = i; ii < i + step_h; ++ii)
              {
                //*((int *) &mvd[0][ii][0]) = *((int *) curr_mvd);
                mvd[0][ii][0] = curr_mvd[0];
                mvd[0][ii][1] = curr_mvd[1];
              }              

              // now copy all other lines
              for(jj = 1; jj < step_v; ++jj)
              {
                memcpy(&mvd[jj][i][0], &mvd[0][i][0],  2 * step_h * sizeof(short));
              }
#endif							
            }
          }
        }
      }
    }

		if(mvd_num > 0)
			write_mvd2keyfile(bit_offset_from_rbsp, key_data_len, mvd_sum, mvd_num);
  }
}

static inline void setup_mb_pos_info(Macroblock *currMB)
{
  int mb_x = currMB->mb.x;
  int mb_y = currMB->mb.y;
  currMB->block_x     = mb_x << BLOCK_SHIFT;           /* horizontal block position */
  currMB->block_y     = mb_y << BLOCK_SHIFT;           /* vertical block position */
  currMB->block_y_aff = currMB->block_y;                       /* interlace relative vertical position */
  currMB->pix_x       = mb_x << MB_BLOCK_SHIFT;        /* horizontal luma pixel position */
  currMB->pix_y       = mb_y << MB_BLOCK_SHIFT;        /* vertical luma pixel position */
  currMB->pix_c_x     = mb_x * currMB->p_Vid->mb_cr_size_x;    /* horizontal chroma pixel position */
  currMB->pix_c_y     = mb_y * currMB->p_Vid->mb_cr_size_y;    /* vertical chroma pixel position */
}

/*!
 ************************************************************************
 * \brief
 *    initializes the current macroblock
 ************************************************************************
 */
void start_macroblock(Slice *currSlice, Macroblock **currMB)
{
  VideoParameters *p_Vid = currSlice->p_Vid;
  int mb_nr = currSlice->current_mb_nr;
  
  *currMB = &currSlice->mb_data[mb_nr]; 

  (*currMB)->p_Slice = currSlice;
  (*currMB)->p_Vid   = p_Vid;  
  (*currMB)->mbAddrX = mb_nr;

  //assert (mb_nr < (int) p_Vid->PicSizeInMbs);

  /* Update coordinates of the current macroblock */
  if (currSlice->mb_aff_frame_flag)
  {
    (*currMB)->mb.x = (short) (   (mb_nr) % ((2*p_Vid->width) / MB_BLOCK_SIZE));
    (*currMB)->mb.y = (short) (2*((mb_nr) / ((2*p_Vid->width) / MB_BLOCK_SIZE)));

    (*currMB)->mb.y += ((*currMB)->mb.x & 0x01);
    (*currMB)->mb.x >>= 1;
  }
  else
  {
    (*currMB)->mb = p_Vid->PicPos[mb_nr];
  }

  /* Define pixel/block positions */
  setup_mb_pos_info(*currMB);

  // reset intra mode
  (*currMB)->is_intra_block = FALSE;
  // reset mode info
  (*currMB)->mb_type         = 0;
  (*currMB)->delta_quant     = 0;
  (*currMB)->cbp             = 0;    
  (*currMB)->c_ipred_mode    = DC_PRED_8;

  // Save the slice number of this macroblock. When the macroblock below
  // is coded it will use this to decide if prediction for above is possible
  (*currMB)->slice_nr = (short) currSlice->current_slice_nr;

  CheckAvailabilityOfNeighbors(*currMB);

  set_read_and_store_CBP(currMB, currSlice->active_sps->chroma_format_idc);

  // Reset syntax element entries in MB struct
  if (currSlice->slice_type != I_SLICE)
  {
    if (currSlice->slice_type != B_SLICE)
      fast_memset((*currMB)->mvd[0][0][0], 0, MB_BLOCK_PARTITIONS * 2 * sizeof(short));
    else
      fast_memset((*currMB)->mvd[0][0][0], 0, 2 * MB_BLOCK_PARTITIONS * 2 * sizeof(short));
  }
  
  fast_memset((*currMB)->s_cbp, 0, 3 * sizeof(CBPStructure));

  // store filtering parameters for this MB
  //(*currMB)->DFDisableIdc    = currSlice->DFDisableIdc;
  //(*currMB)->DFAlphaC0Offset = currSlice->DFAlphaC0Offset;
  //(*currMB)->DFBetaOffset    = currSlice->DFBetaOffset;
  (*currMB)->list_offset     = 0;
}

/*!
 ************************************************************************
 * \brief
 *    set coordinates of the next macroblock
 *    check end_of_slice condition
 ************************************************************************
 */
Boolean exit_macroblock(Slice *currSlice, int eos_bit)
{
  VideoParameters *p_Vid = currSlice->p_Vid;

 //! The if() statement below resembles the original code, which tested
  //! p_Vid->current_mb_nr == p_Vid->PicSizeInMbs.  Both is, of course, nonsense
  //! In an error prone environment, one can only be sure to have a new
  //! picture by checking the tr of the next slice header!

// printf ("exit_macroblock: FmoGetLastMBOfPicture %d, p_Vid->current_mb_nr %d\n", FmoGetLastMBOfPicture(), p_Vid->current_mb_nr);
  ++(currSlice->num_dec_mb);

  if(currSlice->current_mb_nr == p_Vid->PicSizeInMbs - 1) //if (p_Vid->num_dec_mb == p_Vid->PicSizeInMbs)
  {
    return TRUE;
  }
  // ask for last mb in the slice  CAVLC
  else
  {

    currSlice->current_mb_nr = FmoGetNextMBNr (p_Vid, currSlice->current_mb_nr);

    if (currSlice->current_mb_nr == -1)     // End of Slice group, MUST be end of slice
    {
      assert (currSlice->nal_startcode_follows (currSlice, eos_bit) == TRUE);
      return TRUE;
    }

    if(currSlice->nal_startcode_follows(currSlice, eos_bit) == FALSE)
      return FALSE;

    if(currSlice->slice_type == I_SLICE  || currSlice->slice_type == SI_SLICE || p_Vid->active_pps->entropy_coding_mode_flag == (Boolean) CABAC)
      return TRUE;
    if(currSlice->cod_counter <= 0)
      return TRUE;
    return FALSE;
  }
}

/*!
 ************************************************************************
 * \brief
 *    Interpret the mb mode for P-Frames
 ************************************************************************
 */
static void interpret_mb_mode_P(Macroblock *currMB)
{
  static const short ICBPTAB[6] = {0,16,32,15,31,47};
  short  mbmode = currMB->mb_type;

  if(mbmode < 4)
  {
    currMB->mb_type = mbmode;
    memset(currMB->b8mode, mbmode, 4 * sizeof(char));
    memset(currMB->b8pdir, 0, 4 * sizeof(char));
  }
  else if((mbmode == 4 || mbmode == 5))
  {
    currMB->mb_type = P8x8;
    currMB->p_Slice->allrefzero = (mbmode == 5);
  }
  else if(mbmode == 6)
  {
    currMB->is_intra_block = TRUE;
    currMB->mb_type = I4MB;
    memset(currMB->b8mode, IBLOCK, 4 * sizeof(char));
    memset(currMB->b8pdir,     -1, 4 * sizeof(char));
  }
  else if(mbmode == 31)
  {
    currMB->is_intra_block = TRUE;
    currMB->mb_type = IPCM;
    currMB->cbp = -1;
    //currMB->i16mode = 0;

    memset(currMB->b8mode, 0, 4 * sizeof(char));
    memset(currMB->b8pdir,-1, 4 * sizeof(char));
  }
  else
  {
    currMB->is_intra_block = TRUE;
    currMB->mb_type = I16MB;
    currMB->cbp = ICBPTAB[((mbmode-7))>>2];
    //currMB->i16mode = ((mbmode-7)) & 0x03;
    memset(currMB->b8mode, 0, 4 * sizeof(char));
    memset(currMB->b8pdir,-1, 4 * sizeof(char));
  }
}

/*!
 ************************************************************************
 * \brief
 *    Interpret the mb mode for I-Frames
 ************************************************************************
 */
static void interpret_mb_mode_I(Macroblock *currMB)
{
  static const short ICBPTAB[6] = {0,16,32,15,31,47};
  short mbmode   = currMB->mb_type;

  if (mbmode == 0)
  {
    currMB->is_intra_block = TRUE;
    currMB->mb_type = I4MB;
    memset(currMB->b8mode,IBLOCK,4 * sizeof(char));
    memset(currMB->b8pdir,-1,4 * sizeof(char));
  }
  else if(mbmode == 25)
  {
    currMB->is_intra_block = TRUE;
    currMB->mb_type=IPCM;
    currMB->cbp= -1;
    //currMB->i16mode = 0;

    memset(currMB->b8mode, 0,4 * sizeof(char));
    memset(currMB->b8pdir,-1,4 * sizeof(char));
  }
  else
  {
    currMB->is_intra_block = TRUE;
    currMB->mb_type = I16MB;
    currMB->cbp= ICBPTAB[(mbmode-1)>>2];
    //currMB->i16mode = (mbmode-1) & 0x03;
    memset(currMB->b8mode, 0, 4 * sizeof(char));
    memset(currMB->b8pdir,-1, 4 * sizeof(char));
  }
}

/*!
 ************************************************************************
 * \brief
 *    Interpret the mb mode for B-Frames
 ************************************************************************
 */
static void interpret_mb_mode_B(Macroblock *currMB)
{
  static const char offset2pdir16x16[12]   = {0, 0, 1, 2, 0,0,0,0,0,0,0,0};
  static const char offset2pdir16x8[22][2] = {{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},{1,1},{0,0},{0,1},{0,0},{1,0},
                                             {0,0},{0,2},{0,0},{1,2},{0,0},{2,0},{0,0},{2,1},{0,0},{2,2},{0,0}};
  static const char offset2pdir8x16[22][2] = {{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},{1,1},{0,0},{0,1},{0,0},
                                             {1,0},{0,0},{0,2},{0,0},{1,2},{0,0},{2,0},{0,0},{2,1},{0,0},{2,2}};

  static const char ICBPTAB[6] = {0,16,32,15,31,47};

  short i, mbmode;
  short mbtype  = currMB->mb_type;

  //--- set mbtype, b8type, and b8pdir ---
  if (mbtype == 0)       // direct
  {
    mbmode=0;
    memset(currMB->b8mode, 0, 4 * sizeof(char));
    memset(currMB->b8pdir, 2, 4 * sizeof(char));
  }
  else if (mbtype == 23) // intra4x4
  {
    currMB->is_intra_block = TRUE;
    mbmode=I4MB;
    memset(currMB->b8mode, IBLOCK,4 * sizeof(char));
    memset(currMB->b8pdir, -1,4 * sizeof(char));
  }
  else if ((mbtype > 23) && (mbtype < 48) ) // intra16x16
  {
    currMB->is_intra_block = TRUE;
    mbmode=I16MB;
    memset(currMB->b8mode,  0, 4 * sizeof(char));
    memset(currMB->b8pdir, -1, 4 * sizeof(char));

    currMB->cbp     = (int) ICBPTAB[(mbtype-24)>>2];
    //currMB->i16mode = (mbtype-24) & 0x03;
  }
  else if (mbtype == 22) // 8x8(+split)
  {
    mbmode=P8x8;       // b8mode and pdir is transmitted in additional codewords
  }
  else if (mbtype < 4)   // 16x16
  {
    mbmode = 1;
    memset(currMB->b8mode, 1,4 * sizeof(char));
    memset(currMB->b8pdir, offset2pdir16x16[mbtype], 4 * sizeof(char));
  }
  else if(mbtype == 48)
  {
    currMB->is_intra_block = TRUE;
    mbmode=IPCM;
    memset(currMB->b8mode, 0,4 * sizeof(char));
    memset(currMB->b8pdir,-1,4 * sizeof(char));

    currMB->cbp= -1;
    //currMB->i16mode = 0;
  }
  else if ((mbtype & 0x01) == 0) // 16x8
  {
    mbmode = 2;
    memset(currMB->b8mode, 2,4 * sizeof(char));
    for(i=0;i<4;++i)
    {
      currMB->b8pdir[i] = offset2pdir16x8 [mbtype][i>>1];
    }
  }
  else
  {
    mbmode=3;
    memset(currMB->b8mode, 3,4 * sizeof(char));
    for(i=0;i<4; ++i)
    {
      currMB->b8pdir[i] = offset2pdir8x16 [mbtype][i&0x01];
    }
  }
  currMB->mb_type = mbmode;
}

/*!
 ************************************************************************
 * \brief
 *    Interpret the mb mode for SI-Frames
 ************************************************************************
 */
static void interpret_mb_mode_SI(Macroblock *currMB)
{
  //VideoParameters *p_Vid = currMB->p_Vid;
  const int ICBPTAB[6] = {0,16,32,15,31,47};
  short mbmode   = currMB->mb_type;

  if (mbmode == 0)
  {
    currMB->is_intra_block = TRUE;
    currMB->mb_type = SI4MB;
    memset(currMB->b8mode,IBLOCK,4 * sizeof(char));
    memset(currMB->b8pdir,-1,4 * sizeof(char));
    //currMB->p_Slice->siblock[currMB->mb.y][currMB->mb.x]=1;
  }
  else if (mbmode == 1)
  {
    currMB->is_intra_block = TRUE;
    currMB->mb_type = I4MB;
    memset(currMB->b8mode,IBLOCK,4 * sizeof(char));
    memset(currMB->b8pdir,-1,4 * sizeof(char));
  }
  else if(mbmode == 26)
  {
    currMB->is_intra_block = TRUE;
    currMB->mb_type=IPCM;
    currMB->cbp= -1;
    //currMB->i16mode = 0;
    memset(currMB->b8mode,0,4 * sizeof(char));
    memset(currMB->b8pdir,-1,4 * sizeof(char));
  }

  else
  {
    currMB->is_intra_block = TRUE;
    currMB->mb_type = I16MB;
    currMB->cbp= ICBPTAB[(mbmode-2)>>2];
    //currMB->i16mode = (mbmode-2) & 0x03;
    memset(currMB->b8mode,0,4 * sizeof(char));
    memset(currMB->b8pdir,-1,4 * sizeof(char));
  }
}


/*!
 ************************************************************************
 * \brief
 *    Set mode interpretation based on slice type
 ************************************************************************
 */
void setup_slice_methods(Slice *currSlice)
{
  setup_read_macroblock (currSlice);
  switch (currSlice->slice_type)
  {
  case P_SLICE: 
    currSlice->interpret_mb_mode         = interpret_mb_mode_P;
    currSlice->read_motion_info_from_NAL = read_motion_info_from_NAL_p_slice;
    //currSlice->decode_one_component      = decode_one_component_p_slice;
    //currSlice->update_direct_mv_info     = NULL;
#if (MVC_EXTENSION_ENABLE)
    //currSlice->init_lists                = currSlice->view_id ? init_lists_p_slice_mvc : init_lists_p_slice;
#else
    //currSlice->init_lists                = init_lists_p_slice;
#endif
    break;
  case SP_SLICE:
    currSlice->interpret_mb_mode         = interpret_mb_mode_P;
    currSlice->read_motion_info_from_NAL = read_motion_info_from_NAL_p_slice;
    //currSlice->decode_one_component      = decode_one_component_sp_slice;
    //currSlice->update_direct_mv_info     = NULL;
#if (MVC_EXTENSION_ENABLE)
    //currSlice->init_lists                = currSlice->view_id ? init_lists_p_slice_mvc : init_lists_p_slice;
#else
    //currSlice->init_lists                = init_lists_p_slice;
#endif
    break;
  case B_SLICE:
    currSlice->interpret_mb_mode         = interpret_mb_mode_B;
    currSlice->read_motion_info_from_NAL = read_motion_info_from_NAL_b_slice;
    //currSlice->decode_one_component      = decode_one_component_b_slice;
    //update_direct_types(currSlice);
#if (MVC_EXTENSION_ENABLE)
    //currSlice->init_lists                = currSlice->view_id ? init_lists_b_slice_mvc : init_lists_b_slice;
#else
    //currSlice->init_lists                = init_lists_b_slice;
#endif
    break;
  case I_SLICE: 
    currSlice->interpret_mb_mode         = interpret_mb_mode_I;
    currSlice->read_motion_info_from_NAL = NULL;
    //currSlice->decode_one_component      = decode_one_component_i_slice;
    //currSlice->update_direct_mv_info     = NULL;
#if (MVC_EXTENSION_ENABLE)
    //currSlice->init_lists                = currSlice->view_id ? init_lists_i_slice_mvc : init_lists_i_slice;
#else
    //currSlice->init_lists                = init_lists_i_slice;
#endif
    break;
  case SI_SLICE: 
    currSlice->interpret_mb_mode         = interpret_mb_mode_SI;
    currSlice->read_motion_info_from_NAL = NULL;
    //currSlice->decode_one_component      = decode_one_component_i_slice;
    //currSlice->update_direct_mv_info     = NULL;
#if (MVC_EXTENSION_ENABLE)
    //currSlice->init_lists                = currSlice->view_id ? init_lists_i_slice_mvc : init_lists_i_slice;
#else
    //currSlice->init_lists                = init_lists_i_slice;
#endif
    break;
  default:
    printf("Unsupported slice type\n");
    break;
  }

  //set_intra_prediction_modes(currSlice);

  if ( currSlice->p_Vid->active_sps->chroma_format_idc==YUV444 && (currSlice->p_Vid->separate_colour_plane_flag == 0) )
    currSlice->read_coeff_4x4_CAVLC = read_coeff_4x4_CAVLC_444;
  else
    currSlice->read_coeff_4x4_CAVLC = read_coeff_4x4_CAVLC;

  switch(currSlice->p_Vid->active_pps->entropy_coding_mode_flag)
  {
  case CABAC:
    set_read_CBP_and_coeffs_cabac(currSlice);
    break;
  case CAVLC:
    set_read_CBP_and_coeffs_cavlc(currSlice);
    break;
  default:
    printf("Unsupported entropy coding mode\n");
    break;
  }
}


/*!
 ************************************************************************
 * \brief
 *    Get current block spatial neighbors
 ************************************************************************
 */
void get_neighbors(Macroblock *currMB,       // <--  current Macroblock
                   PixelPos   *block,     // <--> neighbor blocks
                   int         mb_x,         // <--  block x position
                   int         mb_y,         // <--  block y position
                   int         blockshape_x  // <--  block width
                   )
{
  int *mb_size = currMB->p_Vid->mb_size[IS_LUMA];
  
  get4x4Neighbour(currMB, mb_x - 1,            mb_y    , mb_size, block    );
  get4x4Neighbour(currMB, mb_x,                mb_y - 1, mb_size, block + 1);
  get4x4Neighbour(currMB, mb_x + blockshape_x, mb_y - 1, mb_size, block + 2);  

  if (mb_y > 0)
  {
    if (mb_x < 8)  // first column of 8x8 blocks
    {
      if (mb_y == 8 )
      {
        if (blockshape_x == MB_BLOCK_SIZE)      
          block[2].available  = 0;
      }
      else if (mb_x + blockshape_x == 8)
      {
        block[2].available = 0;
      }
    }
    else if (mb_x + blockshape_x == MB_BLOCK_SIZE)
    {
      block[2].available = 0;
    }
  }

  if (!block[2].available)
  {
    get4x4Neighbour(currMB, mb_x - 1, mb_y - 1, mb_size, block + 3);
    block[2] = block[3];
  }
}

/*!
 ************************************************************************
 * \brief
 *    Read motion info
 ************************************************************************
 */
static void read_motion_info_from_NAL_p_slice (Macroblock *currMB)
{
  VideoParameters *p_Vid = currMB->p_Vid;
  Slice *currSlice = currMB->p_Slice;

  SyntaxElement currSE;
  DataPartition *dP = NULL;
  const byte *partMap       = assignSE2partition[currSlice->dp_mode];
  short partmode        = ((currMB->mb_type == P8x8) ? 4 : currMB->mb_type);
  int step_h0         = BLOCK_STEP [partmode][0];
  int step_v0         = BLOCK_STEP [partmode][1];

  int j4;
  StorablePicture *dec_picture = currSlice->dec_picture;
  PicMotionParams *mv_info = NULL;

  int list_offset = currMB->list_offset;
  //StorablePicture **list0 = currSlice->listX[LIST_0 + list_offset];
  PicMotionParams **p_mv_info = &dec_picture->mv_info[currMB->block_y];

  //=====  READ REFERENCE PICTURE INDICES =====
  currSE.type = SE_REFFRAME;
  dP = &(currSlice->partArr[partMap[SE_REFFRAME]]);
  
  //  For LIST_0, if multiple ref. pictures, read LIST_0 reference picture indices for the MB ***********
  prepareListforRefIdx (currMB, &currSE, dP, currSlice->num_ref_idx_active[LIST_0], (currMB->mb_type != P8x8) || (!currSlice->allrefzero));
  readMBRefPictureIdx  (&currSE, dP, currMB, p_mv_info, LIST_0, step_v0, step_h0);

  //=====  READ MOTION VECTORS =====
  currSE.type = SE_MVD;
  dP = &(currSlice->partArr[partMap[SE_MVD]]);

  if (p_Vid->active_pps->entropy_coding_mode_flag == (Boolean) CAVLC || dP->bitstream->ei_flag) 
    currSE.mapping = linfo_se;
  else                                                  
    currSE.reading = currSlice->mb_aff_frame_flag ? read_mvd_CABAC_mbaff : read_MVD_CABAC;

  // LIST_0 Motion vectors
  readMBMotionVectors (&currSE, dP, currMB, LIST_0, step_h0, step_v0);

  // record reference picture Ids for deblocking decisions  
}


/*!
************************************************************************
* \brief
*    Read motion info
************************************************************************
*/
static void read_motion_info_from_NAL_b_slice (Macroblock *currMB)
{
  Slice *currSlice = currMB->p_Slice;
  VideoParameters *p_Vid = currMB->p_Vid;
  StorablePicture *dec_picture = currSlice->dec_picture;
  SyntaxElement currSE;
  DataPartition *dP = NULL;
  const byte *partMap = assignSE2partition[currSlice->dp_mode];
  int partmode        = ((currMB->mb_type == P8x8) ? 4 : currMB->mb_type);
  int step_h0         = BLOCK_STEP [partmode][0];
  int step_v0         = BLOCK_STEP [partmode][1];

  int j4, i4;

  int list_offset = currMB->list_offset; 
  //StorablePicture **list0 = currSlice->listX[LIST_0 + list_offset];
  //StorablePicture **list1 = currSlice->listX[LIST_1 + list_offset];
  PicMotionParams **p_mv_info = &dec_picture->mv_info[currMB->block_y];

  //if (currMB->mb_type == P8x8)
    //currSlice->update_direct_mv_info(currMB);   

  //=====  READ REFERENCE PICTURE INDICES =====
  currSE.type = SE_REFFRAME;
  dP = &(currSlice->partArr[partMap[SE_REFFRAME]]);

  //  For LIST_0, if multiple ref. pictures, read LIST_0 reference picture indices for the MB ***********
  prepareListforRefIdx (currMB, &currSE, dP, currSlice->num_ref_idx_active[LIST_0], TRUE);
  readMBRefPictureIdx  (&currSE, dP, currMB, p_mv_info, LIST_0, step_v0, step_h0);

  //  For LIST_1, if multiple ref. pictures, read LIST_1 reference picture indices for the MB ***********
  prepareListforRefIdx (currMB, &currSE, dP, currSlice->num_ref_idx_active[LIST_1], TRUE);
  readMBRefPictureIdx  (&currSE, dP, currMB, p_mv_info, LIST_1, step_v0, step_h0);

  //=====  READ MOTION VECTORS =====
  currSE.type = SE_MVD;
  dP = &(currSlice->partArr[partMap[SE_MVD]]);

  if (p_Vid->active_pps->entropy_coding_mode_flag == (Boolean) CAVLC || dP->bitstream->ei_flag) 
    currSE.mapping = linfo_se;
  else                                                  
    currSE.reading = currSlice->mb_aff_frame_flag ? read_mvd_CABAC_mbaff : read_MVD_CABAC;

  // LIST_0 Motion vectors
  readMBMotionVectors (&currSE, dP, currMB, LIST_0, step_h0, step_v0);
  // LIST_1 Motion vectors
  readMBMotionVectors (&currSE, dP, currMB, LIST_1, step_h0, step_v0);

  // record reference picture Ids for deblocking decisions
}


/*!
 ************************************************************************
 * \brief
 *    Data partitioning: Check if neighboring macroblock is needed for 
 *    CAVLC context decoding, and disable current MB if data partition
 *    is missing.
 ************************************************************************
 */
void check_dp_neighbors (Macroblock *currMB)
{
  VideoParameters *p_Vid = currMB->p_Vid;
  PixelPos up, left;

  p_Vid->getNeighbour(currMB, -1,  0, p_Vid->mb_size[1], &left);
  p_Vid->getNeighbour(currMB,  0, -1, p_Vid->mb_size[1], &up);

  if ((currMB->is_intra_block == FALSE) || (!(p_Vid->active_pps->constrained_intra_pred_flag)) )
  {
    if (left.available)
    {
      currMB->dpl_flag |= p_Vid->mb_data[left.mb_addr].dpl_flag;
    }
    if (up.available)
    {
      currMB->dpl_flag |= p_Vid->mb_data[up.mb_addr].dpl_flag;
    }
  }
}

/*!
 ************************************************************************
 * \brief
 *    change target plane
 *    for 4:4:4 Independent mode
 ************************************************************************
 */
void change_plane_JV( VideoParameters *p_Vid, int nplane, Slice *pSlice)
{
  //Slice *currSlice = p_Vid->currentSlice;
  //p_Vid->colour_plane_id = nplane;
  p_Vid->mb_data = p_Vid->mb_data_JV[nplane];
  p_Vid->dec_picture  = p_Vid->dec_picture_JV[nplane];
  //p_Vid->siblock = p_Vid->siblock_JV[nplane];
  //p_Vid->ipredmode = p_Vid->ipredmode_JV[nplane];
  p_Vid->intra_block = p_Vid->intra_block_JV[nplane];
  if(pSlice)
  {
    pSlice->mb_data = p_Vid->mb_data_JV[nplane];
    pSlice->dec_picture  = p_Vid->dec_picture_JV[nplane];
    //pSlice->siblock = p_Vid->siblock_JV[nplane];
    //pSlice->ipredmode = p_Vid->ipredmode_JV[nplane];
    pSlice->intra_block = p_Vid->intra_block_JV[nplane];
  }
}


