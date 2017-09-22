/******************************************************************************
  A simple program of Hisilicon mpp audio input/output/encoder/decoder implementation.
  Copyright (C), 2010-2021, Hisilicon Tech. Co., Ltd.
 ******************************************************************************
    Modification:  2013-7 Created
******************************************************************************/

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <pthread.h>
#include <errno.h>
#include <signal.h>
#include <netinet/in.h>

#include "sample_comm.h"
#include "acodec.h"
#include "tlv320aic31.h"

//#define SAVELOCAL

#define SOCKETSERVER 1	    
#define SOCKETCLIENT 2
#define MALLOC(pd,type,size) 	(type *)malloc((size));\
															memset((pd), 0, (size))

#define CACHESIZE 12000
#define ENVENERGYINIT 500000
#define ENVVPPINIT 32767
#define VARIANCELIMIT 4000000

struct sockconf{
    int serv_sockfd;
    int clnt_sockfd;
    struct sockaddr_in serv_addr;
    struct sockaddr_in clnt_addr;
    socklen_t clnt_addr_size;
    unsigned short port;
    char * ip;
};

unsigned short sysport = 0;
char * tcpip = NULL;


struct cache
{
	short data[640];
	HI_U32 len;
	int state;
	int sign;
	long energy;
	long variance;
	short vpp;
};
struct cache audiocache[CACHESIZE] = {0};
int head = 0;
int tail = 0;
int envc = 0;
int cutl = 0;

int savecount = 0;

struct environment
{
	long envenergy;
	short envvpp;
}environment;

//send info 
char sendstart[8]="ERCO";
char sendip[16]="192.168.1.195";
char sendsign[16]="myHvite";
char sendinfo[24]={0};




int sockCreate(struct sockconf * conf, int model);
int sockTCPServer(struct sockconf * conf);
int sockTCPClient(struct sockconf * conf);



static PAYLOAD_TYPE_E gs_enPayloadType = PT_LPCM;

//static HI_BOOL gs_bMicIn = HI_FALSE;

static HI_BOOL gs_bAioReSample  = HI_FALSE;
static HI_BOOL gs_bUserGetMode  = HI_FALSE;
static HI_BOOL gs_bAoVolumeCtrl = HI_TRUE;
static AUDIO_SAMPLE_RATE_E enInSampleRate  = AUDIO_SAMPLE_RATE_BUTT;
static AUDIO_SAMPLE_RATE_E enOutSampleRate = AUDIO_SAMPLE_RATE_BUTT;
static HI_U32 u32AencPtNumPerFrm = 0;

short * pdatabuffer[7] = { 0 };
char * wavlists[50] = { 0 };
int writewav = 0;
int readwav = 0;

#define SAMPLE_DBG(s32Ret)\
do{\
    printf("s32Ret=%#x,fuc:%s,line:%d\n", s32Ret, __FUNCTION__, __LINE__);\
}while(0)

/******************************************************************************
* function : PT Number to String
******************************************************************************/
static char* SAMPLE_AUDIO_Pt2Str(PAYLOAD_TYPE_E enType)
{
    if (PT_G711A == enType)  
    {
        return "g711a";
    }
    else if (PT_G711U == enType)  
    {
        return "g711u";
    }
    else if (PT_ADPCMA == enType)  
    {
        return "adpcm";
    }
    else if (PT_G726 == enType) 
    {
        return "g726";
    }
    else if (PT_LPCM == enType)  
    {
        return "pcm";
    }
    else 
    {
        return "data";
    }
}

/******************************************************************************
* function : Open Aenc File
******************************************************************************/
static FILE * SAMPLE_AUDIO_OpenAencFile(AENC_CHN AeChn, PAYLOAD_TYPE_E enType)
{
    FILE* pfd;
    HI_CHAR aszFileName[FILE_NAME_LEN];

    /* create file for save stream*/
    snprintf(aszFileName, FILE_NAME_LEN, "audio_chn%d.%s", AeChn, SAMPLE_AUDIO_Pt2Str(enType));
    pfd = fopen(aszFileName, "w+");
    if (NULL == pfd)
    {
        printf("%s: open file %s failed\n", __FUNCTION__, aszFileName);
        return NULL;
    }
    printf("open stream file:\"%s\" for aenc ok\n", aszFileName);
    return pfd;
}

/******************************************************************************
* function : Open Adec File
******************************************************************************/
static FILE *SAMPLE_AUDIO_OpenAdecFile(ADEC_CHN AdChn, PAYLOAD_TYPE_E enType)
{
    FILE* pfd;
    HI_CHAR aszFileName[FILE_NAME_LEN];

    /* create file for save stream*/
    snprintf(aszFileName, FILE_NAME_LEN ,"audio_chn%d.%s", AdChn, SAMPLE_AUDIO_Pt2Str(enType));
    pfd = fopen(aszFileName, "rb");
    if (NULL == pfd)
    {
        printf("%s: open file %s failed\n", __FUNCTION__, aszFileName);
        return NULL;
    }
    printf("open stream file:\"%s\" for adec ok\n", aszFileName);
    return pfd;
}


int senddata(char * pdata, int datalenth, char * pinfo, int infolenth)
{
		char * psendbuf = malloc(datalenth+infolenth);
		memset(psendbuf, 0, datalenth+infolenth);
    char * psendinfo = psendbuf;
    char * psenddata = psendbuf+infolenth;
    int sendlenth = infolenth + datalenth;
		memcpy(psendinfo, pinfo, infolenth);
    memcpy(psenddata, pdata, datalenth);
		
		struct sockconf sockconf = {0};
		sockconf.port=sysport;
		sockconf.ip=tcpip;
		printf("connect:%s | %d | \n",sockconf.ip,sockconf.port);
		sockCreate(&sockconf, SOCKETCLIENT);
		send( sockconf.serv_sockfd, psendbuf, sendlenth, 0 );
		close(sockconf.serv_sockfd);
		free(psendbuf);
		return 0;
}

void *YX_EnvListen(void *parg)
{
		environment.envenergy = ENVENERGYINIT;
		environment.envvpp = ENVVPPINIT;
		while(1)
		{

		    while(envc == tail) {usleep(50);}
		    if((audiocache[envc].data != NULL))
		    {
		        short * data = audiocache[envc].data;
		        HI_U32 len = audiocache[envc].len;
		        int i = 0;
            long energy = 0;
            short max = data[0];
            short min = data[0];
		        for(i=0;i<len;i++)
		        {
		            (data[i] > 0) ? (energy = energy + data[i]) : (energy = energy - data[i]);
		            if(max < data[i]) {max = data[i];}
		            if(min > data[i]) {min = data[i];}
		            //variance = variance + (data[i] - aver) * (data[i] - aver);
		        }
		        
		        long aver = 0;		        
		        aver = energy / len;
            long variance = 0;
		        for(i=0;i<len;i++)
		        {
		            (data[i] > 0) ? (variance = variance + (data[i] - aver) * (data[i] - aver)) : (variance = variance + ((0-data[i]) - aver) * ((0-data[i]) - aver));
		            //variance = variance + (data[i] - aver) * (data[i] - aver);
		        }
		        audiocache[envc].energy = energy;
		        audiocache[envc].vpp = max - min;
		        audiocache[envc].state = 2;
		        audiocache[envc].variance = variance;
		        //printf("||:check | tail %8d \tenvc %8d \tenergy: %10d \tvpp: %10d \tvariance: %10d \tstate :%2d\n", tail , envc , audiocache[envc].energy, audiocache[envc].vpp, audiocache[envc].variance, audiocache[envc].state);
		    }
		    if(envc % 600 == 0)
		    {
		        printf(">>>>>>:\n");
		        long sumenergy = 0;
		        long sumvpp = 0;
		        int count = 0;
		        int i = 0;
		        for(i=0; i<CACHESIZE; i++)
		        {
		            if(audiocache[i].state == 2)
		            {
		                //printf("%d %d %d %d %d %d\n", i, environment.envenergy, environment.envvpp, audiocache[i].energy, audiocache[i].vpp, audiocache[i].state);
		                if(((environment.envenergy != ENVENERGYINIT)||(environment.envvpp != ENVVPPINIT))&&((audiocache[i].energy > 3*environment.envenergy)||(audiocache[i].vpp > 3*environment.envvpp)))continue;
		                sumenergy = sumenergy + audiocache[i].energy;
		                sumvpp = sumvpp + audiocache[i].vpp;
		                count++;
		            }
		        }
		        printf("envc: %8d %8d %8d %8d\n",envc, sumenergy, sumvpp, count);
		        if(count != 0)
		        {
		        	  environment.envenergy = sumenergy / count;
		            environment.envvpp = sumvpp / count;
		            printf("%8d %8d\n",environment.envenergy,environment.envvpp);
		        }
		    }
		    envc = (envc + 1) % CACHESIZE;
		}
		return 0;
}

void *YX_CutListen(void *parg)
{
		int start = -1;
		int stop = -1;
		int startsign = -1;
		int stopsign = -1;
		int count = -1;
		while(1)
		{
		    while(cutl == envc) {usleep(50);}
		    if((audiocache[cutl].state == 2))
		    {
		        //printf("%12ld\n",audiocache[cutl].variance);
		        if(audiocache[cutl].variance > VARIANCELIMIT)
		        {
		            if(start == -1)
		            {
		                start = cutl;
		                printf("start pass %d\n",start);
		            }
		        }
		        else 
		        {
		            if((start != -1)||(startsign != -1))
		            {
		                stop = cutl;
		                if(((stop - start + CACHESIZE) % CACHESIZE) >= 5)
		                {
		                    if((startsign == -1)&&(stopsign == -1))
		                    {
		                        startsign = start;
		                        stopsign = stop;
		                        printf("||------> cut : %8d %8d\n",startsign,stopsign);
		                        count = 20;
		                        start = -1;
                            stop = -1;
		                    }
		                    else
		                    {
		                        if((count > 0)&&(start != -1)&&(stop != -1) )
		                        {
		                            stopsign = stop;
		                            start = -1;
                                stop = -1;
		                            count = 20;
		                        }
		                    }
		                    printf("||------> count : %8d\n",count);
		                    if(count == 0)
		                    {
		                        int block = (stopsign - startsign + CACHESIZE) % CACHESIZE;
		                        short * buffer = malloc(block * 640);
		                        memset(buffer, 0, block * 640);
		                        short * pwrite = buffer;
		                        int i;
		                        printf("||------> save file : %d - %d\n", startsign, stopsign);
		                        for(i=startsign;i!=stopsign;i = (i + 1) % CACHESIZE)
		                        {
		                            memcpy(pwrite, audiocache[i].data, 640);
		                            pwrite = (short *)((char *)pwrite + 640);
		                        }
		                        
		                        #ifdef SAVELOCAL
		                        char filename[50] = {0};
		                        sprintf(filename,"save%03d.pcm",savecount);
		                        printf("||------> save file : %s\n",filename);
		                        savecount++;
		                        FILE * fp= fopen(filename,"wb");
		                        fwrite(buffer,1,block * 640,fp);
		                        fclose(fp);
		                        #else
		                        
		                        char * sendinfo[100] = {0};
		                        memcpy(sendinfo, sendstart, 8);
		                        memcpy(sendinfo+8, sendip, 16);
		                        memcpy(sendinfo+24, sendsign, 16);
		                        memcpy(sendinfo+40, sendinfo, 24);
		                        senddata(buffer, block * 640, sendinfo, 64);
		                        
		                        #endif
		                        free(buffer);
		                        startsign = -1;
                            stopsign = -1;
		                        
		                    }
		                }
		            }
                if (count >= 0) count--;
		        }
		    }
		    cutl = (cutl + 1) % CACHESIZE;
        
   
		}
		return 0;
}









/***********************************************************************
@Name    : int sockTCPClient(struct sockconf * conf)
@exp     : create server socket.
@para    : struct sockconf * conf -- Set the parameters for socket.
@return  : normal -- 0
           error -- -1
***********************************************************************/
int sockTCPServer(struct sockconf * conf)
{
    int ret = 0;
    conf->serv_sockfd = socket(AF_INET,SOCK_STREAM,0);
    if (conf->serv_sockfd == -1)
    {
        fprintf(stderr,"[%s] : %d : socket()=%d : %s\n",__FUNCTION__,__LINE__,conf->serv_sockfd,strerror(errno));
        return -1;
    }
    memset(&conf->serv_addr, 0, sizeof(conf->serv_addr));
    conf->serv_addr.sin_family = AF_INET;
    conf->serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    conf->serv_addr.sin_port = htons(conf->port);    


    if (bind(conf->serv_sockfd, (struct sockaddr*)&conf->serv_addr, sizeof(conf->serv_addr)) == -1)
    {
        fprintf(stderr,"[%s] : %d : bind()=%d : %s\n",__FUNCTION__,__LINE__,ret,strerror(errno));
        return -1;
    }
	printf("bind\n");
    if (listen(conf->serv_sockfd, 5) == -1)
    {
        fprintf(stderr,"[%s] : %d : listen()=%d : %s\n",__FUNCTION__,__LINE__,ret,strerror(errno));
        return -1;
    }
	printf("listen\n");

    conf->clnt_addr_size = sizeof(conf->clnt_addr);
    conf->clnt_sockfd = accept(conf->serv_sockfd, (struct sockaddr*)&conf->clnt_addr, &conf->clnt_addr_size);
    if (conf->clnt_sockfd == -1)
    {
        fprintf(stderr,"[%s] : %d : accept()=%d : %s\n",__FUNCTION__,__LINE__,ret,strerror(errno));
        return -1;
    }
	printf("accept\n");

    return 0;
}

/***********************************************************************
@Name    : int sockTCPClient(struct sockconf * conf)
@exp     : create client socket.
@para    : struct sockconf * conf -- Set the parameters for socket.
@return  : normal -- 0
           error -- -1
***********************************************************************/
int sockTCPClient(struct sockconf * conf)
{
    int ret = 0;
    int count = 0;
    conf->serv_sockfd = socket(AF_INET,SOCK_STREAM,0);
    if (conf->clnt_sockfd == -1)
    {
        fprintf(stderr,"[%s] : %d : socket()=%d : %s\n",__FUNCTION__,__LINE__,conf->serv_sockfd,strerror(errno));
        return -1;
    }
    memset(&conf->serv_addr, 0, sizeof(conf->serv_addr));
    conf->serv_addr.sin_family = AF_INET;
    conf->serv_addr.sin_addr.s_addr = inet_addr(conf->ip);
    conf->serv_addr.sin_port = htons(conf->port);
    for(count=0;count<10;count++)
    {
    		if(connect(conf->serv_sockfd, (struct sockaddr*)&conf->serv_addr, sizeof(conf->serv_addr)) != -1) break;
    		else
    		{
    			fprintf(stderr,"[%s] : %d : connect()=%d : %s\n",__FUNCTION__,__LINE__,ret,strerror(errno));
    		}
    		sleep(1);
    }

    return 0;
    
    
}



/***********************************************************************
@Name    : int sockCreate(struct sockconf * conf, int model)
@exp     : create server or client socket. 
@para    : struct sockconf * conf -- Set the parameters for socket.
           int model -- Select the socket model.
@return  : normal -- 0
           error -- -1
***********************************************************************/
int sockCreate(struct sockconf * conf, int model)
{
    int ret=0;
    switch(model)
    {
        case SOCKETSERVER:
			(conf->port<=65535)?sockTCPServer(conf):printf("input error\n");
			break;
        case SOCKETCLIENT:
			((conf->port<=65535)&&(conf->ip!=NULL))?sockTCPClient(conf):printf("input error\n");
			break;
        default: ;
    }
    return 0;
}






/******************************************************************************
* function : Ai -> Aenc -> file
*                       -> Adec -> Ao
******************************************************************************/
HI_S32 SAMPLE_AUDIO_AiAenc(HI_VOID)
{
    HI_S32 i, s32Ret;
    AUDIO_DEV   AiDev = SAMPLE_AUDIO_AI_DEV;
    AI_CHN      AiChn;
    AUDIO_DEV   AoDev = SAMPLE_AUDIO_AO_DEV;
    AO_CHN      AoChn = 0;
    ADEC_CHN    AdChn = 0;
    HI_S32      s32AiChnCnt;
	HI_S32      s32AoChnCnt;
    HI_S32      s32AencChnCnt;
    AENC_CHN    AeChn;
    HI_BOOL     bSendAdec = HI_TRUE;
    FILE        *pfd = NULL;
    AIO_ATTR_S stAioAttr;

#ifdef HI_ACODEC_TYPE_TLV320AIC31
    stAioAttr.enSamplerate   = AUDIO_SAMPLE_RATE_32000;
    stAioAttr.enBitwidth     = AUDIO_BIT_WIDTH_16;
    stAioAttr.enWorkmode     = AIO_MODE_I2S_MASTER;
    stAioAttr.enSoundmode    = AUDIO_SOUND_MODE_MONO;
    stAioAttr.u32EXFlag      = 0;
    stAioAttr.u32FrmNum      = 30;
    stAioAttr.u32PtNumPerFrm = SAMPLE_AUDIO_PTNUMPERFRM;
    stAioAttr.u32ChnCnt      = 1;
    stAioAttr.u32ClkSel      = 1;
#else
    stAioAttr.enSamplerate   = AUDIO_SAMPLE_RATE_32000;		//采样率
    stAioAttr.enBitwidth     = AUDIO_BIT_WIDTH_16;			//16位精度
    stAioAttr.enWorkmode     = AIO_MODE_I2S_MASTER;			
    stAioAttr.enSoundmode    = AUDIO_SOUND_MODE_MONO;		
    stAioAttr.u32EXFlag      = 0;							
    stAioAttr.u32FrmNum      = 30;							//30帧缓存
    stAioAttr.u32PtNumPerFrm = SAMPLE_AUDIO_PTNUMPERFRM;	
    stAioAttr.u32ChnCnt      = 1;							
    stAioAttr.u32ClkSel      = 0;							
#endif
    gs_bAioReSample = HI_FALSE;
    enInSampleRate  = AUDIO_SAMPLE_RATE_BUTT;
    enOutSampleRate = AUDIO_SAMPLE_RATE_BUTT;
	u32AencPtNumPerFrm = stAioAttr.u32PtNumPerFrm;
		
    /********************************************
      step 1: config audio codec
    ********************************************/
    s32Ret = SAMPLE_COMM_AUDIO_CfgAcodec(&stAioAttr);
    if (HI_SUCCESS != s32Ret)
    {
        SAMPLE_DBG(s32Ret);
        return HI_FAILURE;
    }
		printf("config audio codec\n");
    /********************************************
      step 2: start Ai
    ********************************************/
    s32AiChnCnt = stAioAttr.u32ChnCnt; 
    s32Ret = SAMPLE_COMM_AUDIO_StartAi(AiDev, s32AiChnCnt, &stAioAttr, enOutSampleRate, gs_bAioReSample, NULL);
    if (s32Ret != HI_SUCCESS)
    {
        SAMPLE_DBG(s32Ret);
        return HI_FAILURE;
    }
		printf("start Ai\n");

   		//创建线程处理帧数据
		AiChn = 0;
		s32Ret = MY_COMM_AUDIO_CreatTrd(AiDev, AiChn);
		if (s32Ret != HI_SUCCESS)
		{
			SAMPLE_DBG(s32Ret);
			return HI_FAILURE;				
		}
		pthread_t ther[2];
		pthread_create(&ther[0], 0, YX_EnvListen, NULL);
		pthread_create(&ther[1], 0, YX_CutListen, NULL);
		printf("create thread end\n");
		while(1) sleep(1);
    s32Ret = SAMPLE_COMM_AUDIO_StopAi(AiDev, s32AiChnCnt, gs_bAioReSample, HI_FALSE);
    if (s32Ret != HI_SUCCESS)
    {
        SAMPLE_DBG(s32Ret);
        return HI_FAILURE;
    }
	
    return HI_SUCCESS;
}






HI_VOID SAMPLE_AUDIO_Usage(HI_VOID)
{
    printf("\n\n/**************************************************************************/\n");
    printf("Ready Go:\n");
}

/******************************************************************************
* function : to process abnormal case
******************************************************************************/
void SAMPLE_AUDIO_HandleSig(HI_S32 signo)
{
    if (SIGINT == signo || SIGTERM == signo)
    {

        SAMPLE_COMM_AUDIO_DestoryAllTrd();
        SAMPLE_COMM_SYS_Exit();
        printf("\033[0;31mprogram exit abnormally!\033[0;39m\n");
    }

    exit(0);
}

/******************************************************************************
* function : main
******************************************************************************/
HI_S32 main(int argc, char *argv[])
{
    HI_S32 s32Ret = HI_SUCCESS;
    HI_CHAR ch;
    HI_BOOL bExit = HI_FALSE;
    VB_CONF_S stVbConf;
    if(argc != 3)
    {
    	printf("please set tcp like \"sample_audio 192.168.1.122 7001\"\n");
    	return 0;
    }
		sysport=atoi(argv[2]);
		tcpip=argv[1];

    signal(SIGINT, SAMPLE_AUDIO_HandleSig);
    signal(SIGTERM, SAMPLE_AUDIO_HandleSig);
    
    memset(&stVbConf, 0, sizeof(VB_CONF_S));
    s32Ret = SAMPLE_COMM_SYS_Init(&stVbConf);
    if (HI_SUCCESS != s32Ret)
    {
        printf("%s: system init failed with %d!\n", __FUNCTION__, s32Ret);
        return HI_FAILURE;
    }
    memset(audiocache, 0, sizeof(struct cache)*CACHESIZE);
    /******************************************
     1 choose the case
    ******************************************/
    while (1)
    {
        SAMPLE_AUDIO_Usage();
				SAMPLE_AUDIO_AiAenc();
        
        if (bExit)
        {
            break;
        }
    }
    
    SAMPLE_COMM_SYS_Exit();
    
    return s32Ret;
}


