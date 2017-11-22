

/******************************************************************************

  ��Ȩ���� (C), 2001-2011, ��Ϊ�������޹�˾

******************************************************************************
    �� �� ��   : frame_store.h
    �� �� ��   : ����
    ��    ��   :
    ��������   :
    ����޸�   :
    ��������   : ����֡�����ģ��


 �޸���ʷ   :
    1.��    �� : 2009-08-24
    ��    ��   :
    �޸�����   :

******************************************************************************/

#ifndef _PUB_FRAME_STORE_HEADER_
#define _PUB_FRAME_STORE_HEADER_

#include "basedef.h"
#include "vfmw.h"


/*================================== ���� ===================================*/
#define   FS_OK     0
#define   FS_ERR   -1

#define   MAX_FS_NUM  64

/*==================================  ��  ===================================*/


/*================================== �ṹ ===================================*/
/* ֡�� */
typedef struct hi_FS_S
{
    SINT8    *pu8VirAddr;
    SINT8     s32RefFlag;    /* 0: ����ȫ���ο���1: ���������ο��� 2: �׳������ο��� 3: ���������ο�  */
    SINT8     s32DispFlag;   /* 0: ����ȫ����ʾ��1: �����ȴ���ʾ�� 2: �׳��ȴ���ʾ�� 3: ����������ʾ  */
    /* Ӧ���Զ�����Ϣ */
    IMAGE     stImage;

    /* �������� */
    SINT32    s32FsID;
    SINT32    s32ErrLevel;

    /* ��ַ */
    SINT32    s32PhyAddr;
} FS_S;


/* ֡��� */
typedef struct hiFS_POOL_S
{
    FS_S      stFrameStore[MAX_FS_NUM];
    SINT32    s32ValidFsNum;
    /* ֡���� */
    SINT32    s32FrameWidth;
    SINT32    s32FrameHeight;
    SINT32    s32FrameStride;
} FS_POOL_S;


/*****************************************************************************************
ԭ��    VOID FS_InitFsPool(FS_POOL_S *pstFsPool, SINT32 FrameWidth, SINT32 FrameHeight, SINT32 FrameStride )
����    ��ʼ��֡��أ�������в�����Ϣ
����    ...
����ֵ  FS_OK, FS_ERR
******************************************************************************************/
SINT32 FS_InitFsPool(FS_POOL_S *pstFsPool, SINT32 s32FrameWidth, SINT32 s32FrameHeight, SINT32 s32FrameStride );

/*****************************************************************************************
ԭ��    SINT32 FS_CreateFrameStore(FS_POOL_S *pstFsPool, SINT32 s32PhyAddr, UINT8 *pu8VirAddr)
����    ��֡����д���һ��֡����Ϣ�����ҷ��ش����ɹ���֡��ı��
����    ...
����ֵ   ���󷵻�-1, ���򷵻�֡��ı��
******************************************************************************************/
SINT32 FS_CreateFrameStore(FS_POOL_S *pstFsPool, SINT32 s32PhyAddr, UINT8 *pu8VirAddr);

/*****************************************************************************************
ԭ��    IMAGE *FS_GetImagePtr (FS_POOL_S *pstFsPool, SINT32 FsID)
����    ��ȡָ��֡��imageָ��
����    ...
����ֵ  �ɹ�����imageָ�룬���򷵻�NULL
******************************************************************************************/
IMAGE *FS_GetImagePtr (FS_POOL_S *pstFsPool, SINT32 FsID);

/*****************************************************************************************
ԭ��    VOID  FS_MarkFsRef(FS_POOL_S *pstFsPool, SINT32 FsID, SINT32 RefFlag)
����    �޸�ָ��֡��Ĳο���־
����    ...
����ֵ  ��
******************************************************************************************/
VOID  FS_MarkReference(FS_POOL_S *pstFsPool, SINT32 FsID, SINT32 RefFlag);

/*****************************************************************************************
ԭ��    VOID FS_MarkFsDisplay(FS_POOL_S *pstFsPool, SINT32 FsID, SINT32 DispFlag)
����    �޸�ָ��֡��Ĳο���־
����    ...
����ֵ  ��
******************************************************************************************/
VOID FS_MarkDisplay(FS_POOL_S *pstFsPool, SINT32 FsID, SINT32 DispFlag);

/*****************************************************************************************
ԭ��    VOID FS_SetErrLevel(FS_POOL_S *pstFsPool, SINT32 FsID, SINT32 ErrLevel)
����    �޸�ָ��֡��Ĵ����ʣ��ڳ��Խ���ʱ�����õ�
����    ...
����ֵ  ��
******************************************************************************************/
VOID FS_SetErrLevel(FS_POOL_S *pstFsPool, SINT32 FsID, SINT32 ErrLevel);

/*****************************************************************************************
ԭ��    SINT32 FS_GetFreeFs(FS_POOL_S *pstFsPool)
����    ��ȡһ�����е�֡��
����    ...
����ֵ  ��
******************************************************************************************/
SINT32 FS_GetFreeFs(FS_POOL_S *pstFsPool);

/*****************************************************************************************
ԭ��    VOID FS_ClearFsPool(FS_POOL_S *pstFsPool)
����    ���֡���������֡���ռ�ñ�־��ʹ֮ȫ�����ڿ���״̬
����    ...
����ֵ  ��
******************************************************************************************/
VOID FS_ClearFsPool(FS_POOL_S *pstFsPool);

/*****************************************************************************************
ԭ��    SINT32  FS_GetTotalNum(FS_POOL_S *pstFsPool)
����    ��ѯ֡����е���Ч֡�����
����    ...
����ֵ  ��Ч֡�����
******************************************************************************************/
SINT32  FS_GetTotalNum(FS_POOL_S *pstFsPool);






#endif


