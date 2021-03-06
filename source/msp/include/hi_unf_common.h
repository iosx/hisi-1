/******************************************************************************

  Copyright (C), 2001-2011, Hisilicon Tech. Co., Ltd.

 ******************************************************************************
  File Name     : hi_unf_disp.h
  Version       : Initial Draft
  Author        : Hisilicon multimedia software group
  Created       : 2009/12/17
  Description   :
  History       :
  1.Date        : 2009/12/17
    Author      : w58735
    Modification: Created file

*******************************************************************************/

#ifndef __HI_UNF_COMMON_H__
#define __HI_UNF_COMMON_H__


#ifdef __cplusplus
#if __cplusplus
extern "C"{
#endif
#endif /* __cplusplus */

#include "hi_common.h"
#include "hi_unf_audio.h"
#include "hi_unf_video.h"

/**Defines the stream buffer.*/
/**CNcomment: 定义码流缓冲结构体 */
typedef struct hiUNF_STREAM_BUF_S
{
    HI_U8   *pu8Data;        /**<Data pointer*/ /**<CNcomment: 数据指针 */
    HI_U32  u32Size;         /**<Data size*/ /**<CNcomment: 数据长度 */
} HI_UNF_STREAM_BUF_S;

/**Defines the structure of the ES buffer.*/
/**CNcomment: 定义ES码流缓冲结构体 */
typedef struct hiUNF_ES_BUF_S
{
    HI_U8  *pu8Buf;    /**<User-state virtual address of the buffer*/ /**<CNcomment: buffer的用户态虚地址*/
    HI_U32 u32BufLen;  /**<Buffer length*/ /**<CNcomment: buffer的长度*/
    HI_U32 u32PtsMs;   /**<Presentation time stamp (PTS) value corresponding to the start of the stream. The invalid value is 0xFFFFFFFF.*/
                       /**<CNcomment: 码流开始处对应的PTS值，无效为0xffffffff*/
}HI_UNF_ES_BUF_S;

/** Defines of video frame crop parameter */
/** CNcomment:视频帧裁剪参数 */
typedef struct hiUNF_CROP_RECT_S
{
    HI_U32 u32LeftOffset;   /**<Left offset of crop*//**<CNcomment:裁剪左偏移*/
    HI_U32 u32TopOffset;    /**<Top offset of crop*//**<CNcomment:裁剪上偏移*/
    HI_U32 u32RightOffset;  /**<Right offset of crop*//**<CNcomment:裁剪右偏移*/
    HI_U32 u32BottomOffset; /**<Bottom offset of crop*//**<CNcomment:裁剪下偏移*/
}HI_UNF_CROP_RECT_S;

/** @} */  /** <!-- ==== Structure Definition End ==== */

#ifdef __cplusplus
#if __cplusplus
}
#endif
#endif /* __cplusplus */

#endif /* __HI_UNF_COMMON_ H*/
