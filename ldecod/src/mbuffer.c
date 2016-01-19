
/*!
 ***********************************************************************
 *  \file
 *      mbuffer.c
 *
 *  \brief
 *      Frame buffer functions
 *
 *  \author
 *      Main contributors (see contributors.h for copyright, address and affiliation details)
 *      - Karsten Suehring
 *      - Alexis Tourapis                 <alexismt@ieee.org>
 *      - Jill Boyce                      <jill.boyce@thomson.net>
 *      - Saurav K Bandyopadhyay          <saurav@ieee.org>
 *      - Zhenyu Wu                       <Zhenyu.Wu@thomson.net
 *      - Purvin Pandit                   <Purvin.Pandit@thomson.net>
 *      - Yuwen He                        <yhe@dolby.com>
 ***********************************************************************
 */

#include <limits.h>

#include "global.h"
#include "header.h"
#include "image.h"
#include "mbuffer.h"
#include "memalloc.h"
#include "fast_memory.h"

static void insert_picture_in_dpb    (VideoParameters *p_Vid, FrameStore* fs, StorablePicture* p);
static int output_one_frame_from_dpb (DecodedPictureBuffer *p_Dpb);
static void gen_field_ref_ids        (VideoParameters *p_Vid, StorablePicture *p);

#define MAX_LIST_SIZE 33

/*!
 ************************************************************************
 * \brief
 *    Returns the size of the dpb depending on level and picture size
 *
 *
 ************************************************************************
 */
int getDpbSize(VideoParameters *p_Vid, seq_parameter_set_rbsp_t *active_sps)
{
  int pic_size_mb = (active_sps->pic_width_in_mbs_minus1 + 1) * (active_sps->pic_height_in_map_units_minus1 + 1) * (active_sps->frame_mbs_only_flag?1:2);

  int size = 0;

  switch (active_sps->level_idc)
  {
  case 0:
    // if there is no level defined, we expect experimental usage and return a DPB size of 16
    return 16;
  case 9:
    size = 396;
    break;
  case 10:
    size = 396;
    break;
  case 11:
    if (!is_FREXT_profile(active_sps->profile_idc) && (active_sps->constrained_set3_flag == 1))
      size = 396;
    else
      size = 900;
    break;
  case 12:
    size = 2376;
    break;
  case 13:
    size = 2376;
    break;
  case 20:
    size = 2376;
    break;
  case 21:
    size = 4752;
    break;
  case 22:
    size = 8100;
    break;
  case 30:
    size = 8100;
    break;
  case 31:
    size = 18000;
    break;
  case 32:
    size = 20480;
    break;
  case 40:
    size = 32768;
    break;
  case 41:
    size = 32768;
    break;
  case 42:
    size = 34816;
    break;
  case 50:
    size = 110400;
    break;
  case 51:
    size = 184320;
    break;
  case 52:
    size = 184320;
    break;
  case 60:
  case 61:
  case 62:
    size = 696320;
    break;
  default:
    error ("undefined level", 500);
    break;
  }

  size /= pic_size_mb;
#if MVC_EXTENSION_ENABLE
  if(p_Vid->profile_idc == MVC_HIGH || p_Vid->profile_idc == STEREO_HIGH)
  {
    int num_views = p_Vid->active_subset_sps->num_views_minus1+1;
    size = imin(2*size, imax(1, RoundLog2(num_views))*16)/num_views;
  }
  else
#endif
  {
    size = imin( size, 16);
  }

  if (active_sps->vui_parameters_present_flag && active_sps->vui_seq_parameters.bitstream_restriction_flag)
  {
    int size_vui;
    if ((int)active_sps->vui_seq_parameters.max_dec_frame_buffering > size)
    {
      error ("max_dec_frame_buffering larger than MaxDpbSize", 500);
    }
    size_vui = imax (1, active_sps->vui_seq_parameters.max_dec_frame_buffering);
#ifdef _DEBUG
    if(size_vui < size)
    {
      printf("Warning: max_dec_frame_buffering(%d) is less than DPB size(%d) calculated from Profile/Level.\n", size_vui, size);
    }
#endif
    size = size_vui;    
  }

  return size;
}

/*!
 ************************************************************************
 * \brief
 *    Allocate memory for decoded picture buffer and initialize with sane values.
 *
 ************************************************************************
 */
void init_dpb(VideoParameters *p_Vid, DecodedPictureBuffer *p_Dpb, int type)
{
  unsigned i; 
  seq_parameter_set_rbsp_t *active_sps = p_Vid->active_sps;

  p_Dpb->p_Vid = p_Vid;
  if (p_Dpb->init_done)
  {
    free_dpb(p_Dpb);
  }

  p_Dpb->size = getDpbSize(p_Vid, active_sps) + p_Vid->p_Inp->dpb_plus[type==2? 1: 0];
  p_Dpb->num_ref_frames = active_sps->num_ref_frames; 

#if (MVC_EXTENSION_ENABLE)
  if ((unsigned int)active_sps->max_dec_frame_buffering < active_sps->num_ref_frames)
#else
  if (p_Dpb->size < active_sps->num_ref_frames)
#endif
  {
    error ("DPB size at specified level is smaller than the specified number of reference frames. This is not allowed.\n", 1000);
  }

  p_Dpb->used_size = 0;
  p_Dpb->last_picture = NULL;

  p_Dpb->ref_frames_in_buffer = 0;
  p_Dpb->ltref_frames_in_buffer = 0;

  p_Dpb->fs = calloc(p_Dpb->size, sizeof (FrameStore*));
  if (NULL==p_Dpb->fs)
    no_mem_exit("init_dpb: p_Dpb->fs");

  p_Dpb->fs_ref = calloc(p_Dpb->size, sizeof (FrameStore*));
  if (NULL==p_Dpb->fs_ref)
    no_mem_exit("init_dpb: p_Dpb->fs_ref");

  p_Dpb->fs_ltref = calloc(p_Dpb->size, sizeof (FrameStore*));
  if (NULL==p_Dpb->fs_ltref)
    no_mem_exit("init_dpb: p_Dpb->fs_ltref");

#if (MVC_EXTENSION_ENABLE)
  p_Dpb->fs_ilref = calloc(1, sizeof (FrameStore*));
  if (NULL==p_Dpb->fs_ilref)
    no_mem_exit("init_dpb: p_Dpb->fs_ilref");
#endif

  for (i = 0; i < p_Dpb->size; i++)
  {
    p_Dpb->fs[i]       = alloc_frame_store();
    p_Dpb->fs_ref[i]   = NULL;
    p_Dpb->fs_ltref[i] = NULL;
    p_Dpb->fs[i]->layer_id = MVC_INIT_VIEW_ID;
#if (MVC_EXTENSION_ENABLE)
    p_Dpb->fs[i]->view_id = MVC_INIT_VIEW_ID;
    p_Dpb->fs[i]->inter_view_flag[0] = p_Dpb->fs[i]->inter_view_flag[1] = 0;
    p_Dpb->fs[i]->anchor_pic_flag[0] = p_Dpb->fs[i]->anchor_pic_flag[1] = 0;
#endif
  }
#if (MVC_EXTENSION_ENABLE)
  if (type == 2)
  {
    p_Dpb->fs_ilref[0] = alloc_frame_store();
    // These may need some cleanups
    p_Dpb->fs_ilref[0]->view_id = MVC_INIT_VIEW_ID;
    p_Dpb->fs_ilref[0]->inter_view_flag[0] = p_Dpb->fs_ilref[0]->inter_view_flag[1] = 0;
    p_Dpb->fs_ilref[0]->anchor_pic_flag[0] = p_Dpb->fs_ilref[0]->anchor_pic_flag[1] = 0;
    // given that this is in a different buffer, do we even need proc_flag anymore?    
  }
  else
    p_Dpb->fs_ilref[0] = NULL;
#endif

  /*
  for (i = 0; i < 6; i++)
  {
  currSlice->listX[i] = calloc(MAX_LIST_SIZE, sizeof (StorablePicture*)); // +1 for reordering
  if (NULL==currSlice->listX[i])
  no_mem_exit("init_dpb: currSlice->listX[i]");
  }
  */
  /* allocate a dummy storable picture */
  if(!p_Vid->no_reference_picture)
  {
    p_Vid->no_reference_picture = alloc_storable_picture (p_Vid, FRAME, p_Vid->width, p_Vid->height, p_Vid->width_cr, p_Vid->height_cr, 1);
    p_Vid->no_reference_picture->top_field    = p_Vid->no_reference_picture;
    p_Vid->no_reference_picture->bottom_field = p_Vid->no_reference_picture;
    p_Vid->no_reference_picture->frame        = p_Vid->no_reference_picture;
  }
  p_Dpb->last_output_poc = INT_MIN;

#if (MVC_EXTENSION_ENABLE)
  p_Dpb->last_output_view_id = -1;
#endif

  p_Vid->last_has_mmco_5 = 0;

  p_Dpb->init_done = 1;

  // picture error concealment
  if(p_Vid->conceal_mode !=0 && !p_Vid->last_out_fs)
    p_Vid->last_out_fs = alloc_frame_store();

}

/*!
 ************************************************************************
 * \brief
 *    Free memory for decoded picture buffer.
 ************************************************************************
 */
void free_dpb(DecodedPictureBuffer *p_Dpb)
{
  VideoParameters *p_Vid = p_Dpb->p_Vid;
  unsigned i;
  if (p_Dpb->fs)
  {
    for (i=0; i<p_Dpb->size; i++)
    {
      free_frame_store(p_Dpb->fs[i]);
    }
    free (p_Dpb->fs);
    p_Dpb->fs=NULL;
  }

  if (p_Dpb->fs_ref)
  {
    free (p_Dpb->fs_ref);
  }
  if (p_Dpb->fs_ltref)
  {
    free (p_Dpb->fs_ltref);
  }

#if (MVC_EXTENSION_ENABLE)
  if (p_Dpb->fs_ilref)
  {
    for (i=0; i<1; i++)
    {
      free_frame_store(p_Dpb->fs_ilref[i]);
    }
    free (p_Dpb->fs_ilref);
    p_Dpb->fs_ilref=NULL;
  }

  p_Dpb->last_output_view_id = -1;
#endif

  p_Dpb->last_output_poc = INT_MIN;

  p_Dpb->init_done = 0;

  // picture error concealment
  if(p_Vid->conceal_mode != 0 || p_Vid->last_out_fs)
      free_frame_store(p_Vid->last_out_fs);

  if(p_Vid->no_reference_picture)
  {
    free_storable_picture(p_Vid->no_reference_picture);
    p_Vid->no_reference_picture = NULL;
  }
}


/*!
 ************************************************************************
 * \brief
 *    Allocate memory for decoded picture buffer frame stores and initialize with sane values.
 *
 * \return
 *    the allocated FrameStore structure
 ************************************************************************
 */
FrameStore* alloc_frame_store(void)
{
  FrameStore *f;

  f = calloc (1, sizeof(FrameStore));
  if (NULL==f)
    no_mem_exit("alloc_frame_store: f");

  f->is_used      = 0;
  f->is_reference = 0;
  f->is_long_term = 0;
  f->is_orig_reference = 0;

  f->is_output = 0;

  f->frame        = NULL;;
  f->top_field    = NULL;
  f->bottom_field = NULL;

  return f;
}

void alloc_pic_motion(PicMotionParamsOld *motion, int size_y, int size_x)
{
  motion->mb_field = calloc (size_y * size_x, sizeof(byte));
  if (motion->mb_field == NULL)
    no_mem_exit("alloc_storable_picture: motion->mb_field");
}

/*!
 ************************************************************************
 * \brief
 *    Allocate memory for a stored picture.
 *
 * \param p_Vid
 *    VideoParameters
 * \param structure
 *    picture structure
 * \param size_x
 *    horizontal luma size
 * \param size_y
 *    vertical luma size
 * \param size_x_cr
 *    horizontal chroma size
 * \param size_y_cr
 *    vertical chroma size
 *
 * \return
 *    the allocated StorablePicture structure
 ************************************************************************
 */
StorablePicture* alloc_storable_picture(VideoParameters *p_Vid, PictureStructure structure, int size_x, int size_y, int size_x_cr, int size_y_cr, int is_output)
{
  seq_parameter_set_rbsp_t *active_sps = p_Vid->active_sps;  

  StorablePicture *s;
  int   nplane;

  //printf ("Allocating (%s) picture (x=%d, y=%d, x_cr=%d, y_cr=%d)\n", (type == FRAME)?"FRAME":(type == TOP_FIELD)?"TOP_FIELD":"BOTTOM_FIELD", size_x, size_y, size_x_cr, size_y_cr);

  s = calloc (1, sizeof(StorablePicture));
  if (NULL==s)
    no_mem_exit("alloc_storable_picture: s");

  if (structure!=FRAME)
  {
    size_y    /= 2;
    size_y_cr /= 2;
  }

  s->PicSizeInMbs = (size_x*size_y)/256;
  s->imgUV = NULL;

  get_mem2Dpel_pad (&(s->imgY), size_y, size_x, p_Vid->iLumaPadY, p_Vid->iLumaPadX);
  s->iLumaStride = size_x+2*p_Vid->iLumaPadX;
  s->iLumaExpandedHeight = size_y+2*p_Vid->iLumaPadY;

  if (active_sps->chroma_format_idc != YUV400)
  {
    get_mem3Dpel_pad(&(s->imgUV), 2, size_y_cr, size_x_cr, p_Vid->iChromaPadY, p_Vid->iChromaPadX);
  }

  s->iChromaStride =size_x_cr + 2*p_Vid->iChromaPadX;
  s->iChromaExpandedHeight = size_y_cr + 2*p_Vid->iChromaPadY;
  s->iLumaPadY   = p_Vid->iLumaPadY;
  s->iLumaPadX   = p_Vid->iLumaPadX;
  s->iChromaPadY = p_Vid->iChromaPadY;
  s->iChromaPadX = p_Vid->iChromaPadX;

  s->separate_colour_plane_flag = p_Vid->separate_colour_plane_flag;

  get_mem2Dmp     ( &s->mv_info, (size_y >> BLOCK_SHIFT), (size_x >> BLOCK_SHIFT));
  alloc_pic_motion( &s->motion , (size_y >> BLOCK_SHIFT), (size_x >> BLOCK_SHIFT));

  if( (p_Vid->separate_colour_plane_flag != 0) )
  {
    for( nplane=0; nplane<MAX_PLANE; nplane++ )
    {
      get_mem2Dmp      (&s->JVmv_info[nplane], (size_y >> BLOCK_SHIFT), (size_x >> BLOCK_SHIFT));
      alloc_pic_motion(&s->JVmotion[nplane] , (size_y >> BLOCK_SHIFT), (size_x >> BLOCK_SHIFT));
    }
  }

  s->pic_num   = 0;
  s->frame_num = 0;
  s->long_term_frame_idx = 0;
  s->long_term_pic_num   = 0;
  s->used_for_reference  = 0;
  s->is_long_term        = 0;
  s->non_existing        = 0;
  s->is_output           = 0;
  s->max_slice_id        = 0;
#if (MVC_EXTENSION_ENABLE)
  s->view_id = -1;
#endif

  s->structure=structure;

  s->size_x = size_x;
  s->size_y = size_y;
  s->size_x_cr = size_x_cr;
  s->size_y_cr = size_y_cr;
  s->size_x_m1 = size_x - 1;
  s->size_y_m1 = size_y - 1;
  s->size_x_cr_m1 = size_x_cr - 1;
  s->size_y_cr_m1 = size_y_cr - 1;

  s->top_field    = p_Vid->no_reference_picture;
  s->bottom_field = p_Vid->no_reference_picture;
  s->frame        = p_Vid->no_reference_picture;

  s->dec_ref_pic_marking_buffer = NULL;

  s->coded_frame  = 0;
  s->mb_aff_frame_flag  = 0;

  s->top_poc = s->bottom_poc = s->poc = 0;
  s->seiHasTone_mapping = 0;

  if(!p_Vid->active_sps->frame_mbs_only_flag && structure != FRAME)
  {
    int i, j;
    for(j = 0; j < MAX_NUM_SLICES; j++)
    {
      for (i = 0; i < 2; i++)
      {
        s->listX[j][i] = calloc(MAX_LIST_SIZE, sizeof (StorablePicture*)); // +1 for reordering
        if (NULL==s->listX[j][i])
        no_mem_exit("alloc_storable_picture: s->listX[i]");
      }
    }
  }

  return s;
}

/*!
 ************************************************************************
 * \brief
 *    Free frame store memory.
 *
 * \param p_Vid
 *    VideoParameters
 * \param f
 *    FrameStore to be freed
 *
 ************************************************************************
 */
void free_frame_store(FrameStore* f)
{
  if (f)
  {
    if (f->frame)
    {
      free_storable_picture(f->frame);
      f->frame=NULL;
    }
    if (f->top_field)
    {
      free_storable_picture(f->top_field);
      f->top_field=NULL;
    }
    if (f->bottom_field)
    {
      free_storable_picture(f->bottom_field);
      f->bottom_field=NULL;
    }
    free(f);
  }
}

void free_pic_motion(PicMotionParamsOld *motion)
{
  if (motion->mb_field)
  {
    free(motion->mb_field);
    motion->mb_field = NULL;
  }
}


/*!
 ************************************************************************
 * \brief
 *    Free picture memory.
 *
 * \param p
 *    Picture to be freed
 *
 ************************************************************************
 */
void free_storable_picture(StorablePicture* p)
{
  int nplane;
  if (p)
  {
    if (p->mv_info)
    {
      free_mem2Dmp(p->mv_info);
      p->mv_info = NULL;
    }
    free_pic_motion(&p->motion);

    if( (p->separate_colour_plane_flag != 0) )
    {
      for( nplane=0; nplane<MAX_PLANE; nplane++ )
      {
        if (p->JVmv_info[nplane])
        {
          free_mem2Dmp(p->JVmv_info[nplane]);
          p->JVmv_info[nplane] = NULL;
        }
        free_pic_motion(&p->JVmotion[nplane]);
      }
    }

    if (p->imgY)
    {
      free_mem2Dpel_pad(p->imgY, p->iLumaPadY, p->iLumaPadX);
      p->imgY = NULL;
    }

    if (p->imgUV)
    {
      free_mem3Dpel_pad(p->imgUV, 2, p->iChromaPadY, p->iChromaPadX);
      p->imgUV=NULL;
    }


    if (p->seiHasTone_mapping)
      free(p->tone_mapping_lut);

    {
      int i, j;
      for(j = 0; j < MAX_NUM_SLICES; j++)
      {
        for(i=0; i<2; i++)
        {
          if(p->listX[j][i])
          {
            free(p->listX[j][i]);
            p->listX[j][i] = NULL;
          }
        }
      }
    }
    free(p);
    p = NULL;
  }
}


#if (MVC_EXTENSION_ENABLE)
int GetMaxDecFrameBuffering(VideoParameters *p_Vid)
{
  int i, j, iMax, iMax_1 = 0, iMax_2 = 0;
  subset_seq_parameter_set_rbsp_t *curr_subset_sps;
  seq_parameter_set_rbsp_t *curr_sps;

  curr_subset_sps = p_Vid->SubsetSeqParSet;
  curr_sps = p_Vid->SeqParSet;
  for(i=0; i<MAXSPS; i++)
  {
    if(curr_subset_sps->Valid && curr_subset_sps->sps.seq_parameter_set_id < MAXSPS)
    {
      j = curr_subset_sps->sps.max_dec_frame_buffering;

      if (curr_subset_sps->sps.vui_parameters_present_flag && curr_subset_sps->sps.vui_seq_parameters.bitstream_restriction_flag)
      {
        if ((int)curr_subset_sps->sps.vui_seq_parameters.max_dec_frame_buffering > j)
        {
          error ("max_dec_frame_buffering larger than MaxDpbSize", 500);
        }
        j = imax (1, curr_subset_sps->sps.vui_seq_parameters.max_dec_frame_buffering);
      }

      if(j > iMax_2)
        iMax_2 = j;
    }
    
    if(curr_sps->Valid)
    {
      j = curr_sps->max_dec_frame_buffering;

      if (curr_sps->vui_parameters_present_flag && curr_sps->vui_seq_parameters.bitstream_restriction_flag)
      {
        if ((int)curr_sps->vui_seq_parameters.max_dec_frame_buffering > j)
        {
          error ("max_dec_frame_buffering larger than MaxDpbSize", 500);
        }
        j = imax (1, curr_sps->vui_seq_parameters.max_dec_frame_buffering);
      }

      if(j > iMax_1)
        iMax_1 = j;
    }
    curr_subset_sps++;
    curr_sps++;
  }  
      
  if (iMax_1 > 0 && iMax_2 > 0)
    iMax = iMax_1 + iMax_2;
  else
    iMax = (iMax_1 >0? iMax_1*2 : iMax_2*2);
  return iMax;
}
#endif

int init_img_data(VideoParameters *p_Vid, ImageData *p_ImgData, seq_parameter_set_rbsp_t *sps)
{
  InputParameters *p_Inp = p_Vid->p_Inp;
  int memory_size = 0;
  int nplane;
  
  // allocate memory for reference frame buffers: p_ImgData->frm_data
  p_ImgData->format           = p_Inp->output;
  p_ImgData->format.width[0]  = p_Vid->width;    
  p_ImgData->format.width[1]  = p_Vid->width_cr;
  p_ImgData->format.width[2]  = p_Vid->width_cr;
  p_ImgData->format.height[0] = p_Vid->height;  
  p_ImgData->format.height[1] = p_Vid->height_cr;
  p_ImgData->format.height[2] = p_Vid->height_cr;
  p_ImgData->format.yuv_format          = (ColorFormat) sps->chroma_format_idc;
  p_ImgData->format.auto_crop_bottom    = p_Inp->output.auto_crop_bottom;
  p_ImgData->format.auto_crop_right     = p_Inp->output.auto_crop_right;
  p_ImgData->format.auto_crop_bottom_cr = p_Inp->output.auto_crop_bottom_cr;
  p_ImgData->format.auto_crop_right_cr  = p_Inp->output.auto_crop_right_cr;
  p_ImgData->frm_stride[0]    = p_Vid->width;
  p_ImgData->frm_stride[1]    = p_ImgData->frm_stride[2] = p_Vid->width_cr;
  p_ImgData->top_stride[0] = p_ImgData->bot_stride[0] = p_ImgData->frm_stride[0] << 1;
  p_ImgData->top_stride[1] = p_ImgData->top_stride[2] = p_ImgData->bot_stride[1] = p_ImgData->bot_stride[2] = p_ImgData->frm_stride[1] << 1;

  if( sps->separate_colour_plane_flag )
  {
    for( nplane=0; nplane < MAX_PLANE; nplane++ )
    {
      memory_size += get_mem2Dpel(&(p_ImgData->frm_data[nplane]), p_Vid->height, p_Vid->width);
    }
  }
  else
  {
    memory_size += get_mem2Dpel(&(p_ImgData->frm_data[0]), p_Vid->height, p_Vid->width);

    if (p_Vid->yuv_format != YUV400)
    {
      int i, j, k;
      memory_size += get_mem2Dpel(&(p_ImgData->frm_data[1]), p_Vid->height_cr, p_Vid->width_cr);
      memory_size += get_mem2Dpel(&(p_ImgData->frm_data[2]), p_Vid->height_cr, p_Vid->width_cr);

      if (sizeof(imgpel) == sizeof(unsigned char))
      {
        for (k = 1; k < 3; k++)
          fast_memset(p_ImgData->frm_data[k][0], 128, p_Vid->height_cr * p_Vid->width_cr * sizeof(imgpel));
      }
      else
      {
        imgpel mean_val;

        for (k = 1; k < 3; k++)
        {
          mean_val = (imgpel) ((p_Vid->max_pel_value_comp[k] + 1) >> 1);

          for (j = 0; j < p_Vid->height_cr; j++)
          {
            for (i = 0; i < p_Vid->width_cr; i++)
              p_ImgData->frm_data[k][j][i] = mean_val;
          }
        }
      }
    }
  }

  if (!p_Vid->active_sps->frame_mbs_only_flag)
  {
    // allocate memory for field reference frame buffers
    memory_size += init_top_bot_planes(p_ImgData->frm_data[0], p_Vid->height, &(p_ImgData->top_data[0]), &(p_ImgData->bot_data[0]));

    if (p_Vid->yuv_format != YUV400)
    {
      memory_size += 4*(sizeof(imgpel**));

      memory_size += init_top_bot_planes(p_ImgData->frm_data[1], p_Vid->height_cr, &(p_ImgData->top_data[1]), &(p_ImgData->bot_data[1]));
      memory_size += init_top_bot_planes(p_ImgData->frm_data[2], p_Vid->height_cr, &(p_ImgData->top_data[2]), &(p_ImgData->bot_data[2]));
    }
  }

  return memory_size;
}

void free_img_data(VideoParameters *p_Vid, ImageData *p_ImgData)
{
  if ( p_Vid->separate_colour_plane_flag )
  {
    int nplane;

    for( nplane=0; nplane<MAX_PLANE; nplane++ )
    {
      if (p_ImgData->frm_data[nplane])
      {
        free_mem2Dpel(p_ImgData->frm_data[nplane]);      // free ref frame buffers
        p_ImgData->frm_data[nplane] = NULL;
      }
    }
  }
  else
  {
    if (p_ImgData->frm_data[0])
    {
      free_mem2Dpel(p_ImgData->frm_data[0]);      // free ref frame buffers
      p_ImgData->frm_data[0] = NULL;
    }
    
    if (p_ImgData->format.yuv_format != YUV400)
    {
      if (p_ImgData->frm_data[1])
      {
        free_mem2Dpel(p_ImgData->frm_data[1]);
        p_ImgData->frm_data[1] = NULL;
      }
      if (p_ImgData->frm_data[2])
      {
        free_mem2Dpel(p_ImgData->frm_data[2]);
        p_ImgData->frm_data[2] = NULL;
      }
    }
  }
  
  if (!p_Vid->active_sps->frame_mbs_only_flag)
  {
    free_top_bot_planes(p_ImgData->top_data[0], p_ImgData->bot_data[0]);

    if (p_ImgData->format.yuv_format != YUV400)
    {
      free_top_bot_planes(p_ImgData->top_data[1], p_ImgData->bot_data[1]);
      free_top_bot_planes(p_ImgData->top_data[2], p_ImgData->bot_data[2]);
    }
  }
}

static inline void copy_img_data(imgpel *out_img, imgpel *in_img, int ostride, int istride, unsigned int size_y, unsigned int size_x)
{
  unsigned int i;
  for(i = 0; i < size_y; i++)
  {
    fast_memcpy(out_img, in_img, size_x);
    out_img += ostride;
    in_img += istride;
  }
}

