#include <jni.h>
#include <string>
#include <libusbi.h>
#include <android/log.h>
#include "fileinfo.h"
#include "JavaVm.h"
#include <errno.h>
#include <vector>
#include <sys/stat.h>
#include <string>
#include <chrono>
#include <string.h>
#include <unistd.h>

#define TAG "glsusb"
#define BUF_SIZE    (8192*8*4)
#define TIMEOUT     1000

typedef enum{
    NOT_DEF=-1,
    STREAM_MODE,
    FILE_MODE
} Mode;

typedef struct{
    std::chrono::high_resolution_clock::time_point start;
    std::chrono::high_resolution_clock::time_point stop;
}StopWatch;

typedef struct{
    size_t bytes;
    std::chrono::high_resolution_clock::time_point now;
}ByteSec;

typedef struct{
    bool run;
    pthread_t id;
} Thread;

typedef struct{
    std::string name;
    long size;
    int fd;
} MetaInfo;

static libusb_device_handle *gDevh = NULL;
static unsigned int gCount;
static unsigned char gEpIN = 0x82;   //Input EP
static unsigned char gEpOut = 0x02;   //Output EP
static unsigned char gSync[4] = {0x07, 0x3a, 0xb6, 0x99 };
static Mode gMode = NOT_DEF;
static const char *gClassName = "com/example/gstreamer/MainActivity";
static JavaVM *gJavaVM = NULL;
static jmethodID gOnMessage = NULL;
static jmethodID gOnFileStartRecceivingCB = NULL;
static jmethodID gOnFileReceivedCB = NULL;
static jmethodID gOnFileSentCB = NULL;
static jmethodID gOnAllFilesSentCB = NULL;
static jmethodID gOnFileProgressCB = NULL;
static jmethodID gOnFileName = NULL;
static jobject gObject = NULL;
static std::vector<MetaInfo> gFileList;
static StopWatch gRcvWatch;
static size_t gBytes;
static ByteSec gPrev;
static Thread gRcv,gSnd;
static int gZingMode;
static std::string gFwVersion;

static std::string KMG(unsigned int val)
{
    const int K = 1000;
    char buf[32];
    if(val>pow(K,3)) sprintf(buf,"%.1fG",float(val)/float(pow(K,3)));
    else if(val>pow(K,2)) sprintf(buf,"%.1fM",float(val)/float(pow(K,2)));
    else if(val>K) sprintf(buf,"%.1fK",float(val)/float(K));
    else sprintf(buf,"%d",val);

    return std::string(buf);
}

static std::string commas(std::string number)
{
    int n = number.length()-3;
    while(n>0) {
        number.insert(n,",");
        n-=3;
    }
    return number;
}

static std::string elapsedTime(std::chrono::nanoseconds ns)
{
    return commas(std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(ns).count()))+std::string("ms");
}

static float BpsVal(unsigned int size,float sec)
{
    return (float )size/sec;
}

static std::string Bps(unsigned int size,float sec)
{
    return KMG((unsigned int)BpsVal(size,sec))+std::string("Bps");
}

static std::string bps(unsigned int size, float sec)
{
    return KMG(8*(int)BpsVal(size,sec))+std::string("bps");
}

static std::string stripPath(std::string pathName)
{
    size_t found = pathName.rfind("/");
    if(found!=std::string::npos) {
        return pathName.substr(found+1);    //1 is to exclude starting '/'
    }
    return pathName;
}

static jclass findClass(JNIEnv *env,const char *className)
{
    return env->FindClass(className);
}

static jmethodID getMethod(JNIEnv *env,jclass clazz, const char *funcName, const char *signature)
{
    return env->GetMethodID(clazz,funcName,signature);
}

static int deviceInfo(libusb_device_handle *h)
{
    libusb_device *dev = libusb_get_device(h);
    struct libusb_device_descriptor desc;
    unsigned char string[256];

    int r = libusb_get_device_descriptor(dev,&desc);
    __android_log_print(ANDROID_LOG_INFO,TAG,"libusb_get_device_descriptor = %d",r);
    if(r<0) return r;

    if(desc.iManufacturer) {
        if(libusb_get_string_descriptor_ascii(gDevh, desc.iManufacturer, string, sizeof(string)) > 0)
            __android_log_print(ANDROID_LOG_INFO,TAG,"Manufacturer: %s",(char*)string);
    }

    if(desc.iProduct) {
        if(libusb_get_string_descriptor_ascii(gDevh, desc.iProduct, string, sizeof(string)) > 0)
            __android_log_print(ANDROID_LOG_INFO,TAG,"Product: %s",(char*)string);
    }

    if(desc.iSerialNumber) {
        if(libusb_get_string_descriptor_ascii(gDevh, desc.iSerialNumber, string, sizeof(string)) > 0)
            __android_log_print(ANDROID_LOG_INFO,TAG,"Serial Number: %s",(char*)string);
    }
    return 0;
}

static bool isInputEP(unsigned char ep)
{
    if(ep&0x80) return true;
    return false;
}

static bool syncFound(unsigned char *buf,int length)
{
    if(length<sizeof(gSync)) return false;
    if(memcmp(buf, gSync, sizeof(gSync)) == 0) return true;
    return false;
}

static void convertNameFromUnicodeToAscii(unsigned char *buffer, int bufferSize, char *targetName)
{
    int i = 0;
    for(int j=0;j<bufferSize;j+=2) targetName[i++] = buffer[j];
}

static int nameSize(unsigned char *buffer)
{
    int windowsUnicodeSize;
    memcpy(&windowsUnicodeSize, buffer, sizeof(int));
    return windowsUnicodeSize;
}

static wchar_t toLinUnicode(unsigned char uc1,unsigned char uc2)
{
    return (uc2<<8)|uc1;
}

static std::wstring buf2wstr(unsigned char *buf,unsigned int bufSize)
{
    std::wstring s;
    for(int i=0;i<bufSize;i+=2) s.append(1,toLinUnicode(buf[i],buf[i+1]));
    return s;
}

static unsigned int wstr2buf(std::wstring s,unsigned char *buf,unsigned int bufSize)
{
    assert(2*s.length()<=bufSize);
    for(size_t i=0;i<s.length();i++) {
        buf[2*i] = s[i]&0xff;
        buf[2*i+1] = (s[i]&0xff00)>>8;
    }
    return 2*s.length();
}

static int getFileInfo(unsigned char *buffer, int bufferSize, int syncSize, FILEINFO &info)
{
    int nOffset = syncSize;
    info.name_.clear();
    memcpy(&info.index_, buffer + nOffset, sizeof(int)); nOffset += sizeof(int);
    memcpy(&info.files_, buffer + nOffset, sizeof(int)); nOffset += sizeof(int);

    int size = nameSize(buffer + nOffset); nOffset += sizeof(int);
    info.name_ = buf2wstr(buffer+nOffset,size); nOffset += size;

    memcpy(&info.size_, buffer + nOffset, sizeof(unsigned int)); nOffset += sizeof(unsigned int);

    assert(nOffset <= bufferSize);	//len보다 작거나 같다는 가정
    return nOffset;
}

static void onFileClose(FILE *pFile,FILEINFO *pInfo)
{
    fclose(pFile);
    gRcvWatch.stop = std::chrono::high_resolution_clock::now();

    JavaVm v(gJavaVM);
    if(v.getEnv(JNI_VERSION_1_4)){
        __android_log_print(ANDROID_LOG_INFO,TAG,"Attached to current thread");
    }else{
        __android_log_print(ANDROID_LOG_ERROR,TAG,"failed to attach to current thread");
        return;
    }

    std::string name;
    name.assign(pInfo->name_.begin(),pInfo->name_.end());
    jstring js = v.m_env->NewStringUTF(name.c_str());
    v.m_env->CallVoidMethod(gObject,gOnFileReceivedCB,js);
    float sec = std::chrono::duration_cast<std::chrono::milliseconds>(gRcvWatch.stop-gRcvWatch.start).count()/1000.;
    js = v.m_env->NewStringUTF((commas(std::to_string(pInfo->size_))+"Bytes "+elapsedTime(gRcvWatch.stop-gRcvWatch.start)
                                        +" "+Bps(pInfo->size_,sec)+"("+bps(pInfo->size_,sec)+")").c_str());
    v.m_env->CallVoidMethod(gObject,gOnMessage,js);
}

static int percent(float num,float denom)
{
    return (num/denom)*100;
}

static std::string fileOrder(int zeroBasedOrder,int totalFiles)
{
    std::string strOrder("[");
    strOrder+= std::to_string(zeroBasedOrder+1);
    strOrder+="/";
    strOrder+=std::to_string(totalFiles);
    strOrder+="] ";
    return strOrder;
}

static void* readerThread(void *arg)
{
    int r;
    int transferred = 0;
    unsigned char ep = *((unsigned char*)arg);

    unsigned char *buf = new unsigned char[BUF_SIZE];

    JavaVm v(gJavaVM);
    assert(v.getEnv(JNI_VERSION_1_4));

    libusb_clear_halt(gDevh, ep);
    __android_log_print(ANDROID_LOG_INFO,TAG,"readerThread starts(ep:0x%x)...",ep);
    memset(buf,'\0',BUF_SIZE);
    gCount = 0;

    gBytes = 0;
    size_t bytes = 0;
    FILEINFO info;
    FILE *pFile = 0;
    while(gRcv.run){
        r = libusb_bulk_transfer(gDevh, ep, buf, sizeof(unsigned char) * BUF_SIZE, &transferred, TIMEOUT);
        if(r==0){
            gBytes += transferred;
            gCount++;
#ifdef DEBUG
            __android_log_print(ANDROID_LOG_INFO, TAG, "%u %dbytes", gCount, transferred);
#endif
            if(isInputEP(ep)&&syncFound(buf,sizeof(gSync))) {
                __android_log_print(ANDROID_LOG_INFO,TAG,"InputEP(0x%x) Sync found",ep);

                bytes = 0;
                memset(&info,0,sizeof(info));
                getFileInfo(buf, transferred, sizeof(gSync), info);
                __android_log_print(ANDROID_LOG_INFO,TAG,"index:%d",info.index_);
                __android_log_print(ANDROID_LOG_INFO,TAG,"files:%d",info.files_);
                __android_log_print(ANDROID_LOG_INFO,TAG,"nameSize:%lu",info.name_.size());
                __android_log_print(ANDROID_LOG_INFO,TAG,"%S",info.name_.c_str());
                __android_log_print(ANDROID_LOG_INFO,TAG,"size:%u",info.size_);

                std::string name;
                name.assign(info.name_.begin(),info.name_.end());
                int fd = v.m_env->CallIntMethod(gObject,gOnFileName,v.m_env->NewStringUTF(name.c_str()));
                __android_log_print(ANDROID_LOG_INFO,TAG,"fd=%d",fd);

                pFile = fdopen(fd,"w");
                if(pFile) {
                    __android_log_print(ANDROID_LOG_INFO, TAG, "fopen(%d) ok",fd);
                    std::wstring ws;
                    std::string s = fileOrder(info.index_,info.files_)+std::string("Receiving '");
                    ws.assign(s.begin(),s.end());
                    ws += info.name_;

                    s = std::string("'");
                    std::wstring wsTmp;
                    wsTmp.assign(s.begin(),s.end());
                    ws += wsTmp;

                    jstring js = v.m_env->NewString(reinterpret_cast<const jchar *>(ws.c_str()),2*ws.length());
                    v.m_env->CallVoidMethod(gObject,gOnMessage,js);
                    v.m_env->CallVoidMethod(gObject,gOnFileStartRecceivingCB,NULL);
                    gRcvWatch.start = std::chrono::high_resolution_clock::now();
                }else {
                    __android_log_print(ANDROID_LOG_INFO,TAG,"fdopen(%d) failed, error=%s",fd,strerror(errno));
                    jstring js = v.m_env->NewStringUTF( ("write mode fdopen() error="+std::string(strerror(errno))).c_str());
                    v.m_env->CallVoidMethod(gObject,gOnMessage,js);
                }
            }else{
                if(pFile) {
#ifdef DEBUG
                    __android_log_print(ANDROID_LOG_INFO,TAG,"bytes: %zu received: %d",bytes,transferred);
#endif
                    if(bytes+transferred <= info.size_) {
                        size_t szWrite = fwrite(buf,1,transferred,pFile);
#ifdef DEBUG
                        __android_log_print(ANDROID_LOG_INFO,TAG,"bytes: %zu written to file",szWrite);
#endif
                        assert(szWrite==transferred);
                        bytes += transferred;
                        if(bytes == info.size_) {
                            onFileClose(pFile,&info);
                            pFile = NULL;
                        }
                    }else if(bytes+transferred > info.size_) {
                        size_t szWrite = fwrite(buf,1,info.size_-bytes,pFile);
                        assert(szWrite==(info.size_-bytes));
#ifdef DEBUG
                        __android_log_print(ANDROID_LOG_INFO,TAG,"bytes: %zu written to file",szWrite);
#endif
                        bytes += (info.size_-bytes);
                        assert(bytes==info.size_);
                        onFileClose(pFile,&info);
                        pFile = NULL;
                    }
#ifdef DEBUG
                    __android_log_print(ANDROID_LOG_INFO,TAG,"file:%S bytes/Total= %zu/%u",info.name_.c_str(),bytes,info.size_);
#endif
                    v.m_env->CallVoidMethod(gObject,gOnFileProgressCB,percent(bytes,info.size_));
                }else{
                    bytes += transferred;
                }
            }
        }else{
            //__android_log_print(ANDROID_LOG_ERROR,TAG,"libusb_bulk_transfer=%d",r);
        }
    }
    delete [] buf;
    return ((void*)0);
}

static void convertNameFromAsciiToUnicode(unsigned char *buffer, int bufferSize, char *targetName)
{
    int j = 0;
    memset(targetName,0,2*bufferSize);
    for(int i=0;i<bufferSize;i++) {
        targetName[j] = buffer[i];
        j+=2;
    }
}

static int SetFileInfo(unsigned char *buf, int bufSize, unsigned  char *sync, int syncSize, FILEINFO info)
{
    int nOffset = 0;
    memcpy(buf + nOffset, sync, syncSize); nOffset += syncSize;
    memcpy(buf + nOffset, &info.index_, sizeof(int)); nOffset += sizeof(int);
    memcpy(buf + nOffset, &info.files_, sizeof(int)); nOffset += sizeof(int);

    int modifiedSize = info.name_.length()*2;
    memcpy(buf + nOffset, &modifiedSize, sizeof(int)); nOffset += sizeof(int);
    wstr2buf(info.name_,buf + nOffset,modifiedSize);
    nOffset+= modifiedSize;

    memcpy(buf + nOffset, &info.size_, sizeof(unsigned int)); nOffset += sizeof(unsigned int);

    assert(nOffset <= bufSize);	//len보다 작거나 같다는 가정
    return nOffset;
}

static void onFileSent(FILE *pFile,MetaInfo mi)
{
    fclose(pFile);
    __android_log_print(ANDROID_LOG_INFO,TAG,"file:%s sent",mi.name.c_str());

    JavaVm v(gJavaVM);
    if(v.getEnv(JNI_VERSION_1_4)){
        __android_log_print(ANDROID_LOG_INFO,TAG,"Attached to current thread");
    }else{
        __android_log_print(ANDROID_LOG_ERROR,TAG,"failed to attach to current thread");
        return;
    }

    jstring js = v.m_env->NewStringUTF(stripPath(mi.name.c_str()).c_str());
    v.m_env->CallVoidMethod(gObject,gOnFileSentCB,js);
}

static void send(unsigned char ep,unsigned char *buf,int bufSize)
{
    int r,transferred=0;
    gCount = 0;

    JavaVm v(gJavaVM);
    assert(v.getEnv(JNI_VERSION_1_4));

    size_t szRead;
    size_t bytes = 0;
    while(gSnd.run){
        r = libusb_bulk_transfer(gDevh, ep, buf, sizeof(unsigned char) * BUF_SIZE, &transferred, TIMEOUT);
        if(r==0){
            gCount++;
            gBytes+= transferred;
        }else{
            __android_log_print(ANDROID_LOG_ERROR,TAG,"libusb_bulk_transfer=%d",r);
        }
    }
}

static bool sendFile(unsigned char ep,unsigned char *buf,int bufSize,FILEINFO *pInfo,MetaInfo mi)
{
    __android_log_print(ANDROID_LOG_INFO,TAG,"sendFile ep=0x%x filename=%s",ep,mi.name.c_str());
    int r,transferred=0;
    gCount = 0;

    JavaVm v(gJavaVM);
    assert(v.getEnv(JNI_VERSION_1_4));

    FILE *pFile = fdopen(mi.fd,"r");
    if(pFile) {
        __android_log_print(ANDROID_LOG_INFO, TAG, "fdopen(%s) ok", mi.name.c_str());
    }else {
        __android_log_print(ANDROID_LOG_ERROR,TAG,"fdopen(%s) failed, error=%s",mi.name.c_str(),strerror(errno));
        jstring js = v.m_env->NewStringUTF( ("read mode fdopen("+mi.name+") error="+std::string(strerror(errno))).c_str());
        v.m_env->CallVoidMethod(gObject,gOnMessage,js);
        return false;
    }

    //Send FILEINFO first
    r = SetFileInfo(buf,bufSize,gSync,sizeof(gSync),*pInfo);
    __android_log_print(ANDROID_LOG_INFO, TAG, "SetFileInfo %d bytes", r);
    if((r=libusb_bulk_transfer(gDevh, ep, buf, sizeof(unsigned char) * BUF_SIZE, &transferred, 0))!=0) {
        __android_log_print(ANDROID_LOG_ERROR,TAG,"libusb_bulk_transfer=%d",r);
        return false;
    }
    gCount++;
    gBytes += transferred;
    __android_log_print(ANDROID_LOG_INFO, TAG, "File Info sent %d bytes", BUF_SIZE);

    size_t szRead;
    size_t bytes = 0;
    while(1){
        szRead = fread(buf,1,bufSize,pFile);
        if(szRead==0) {
            assert(feof(pFile));
            __android_log_print(ANDROID_LOG_ERROR,TAG,"fread=%zu",szRead);
            onFileSent(pFile,mi);
            return true;
        }

        r = libusb_bulk_transfer(gDevh, ep, buf, sizeof(unsigned char) * BUF_SIZE, &transferred, 0);
        if(r==0){
            gCount++;
            gBytes+= transferred;
            bytes += szRead;
            __android_log_print(ANDROID_LOG_INFO,TAG,"file:%S bytes/Total= %zu/%u",pInfo->name_.c_str(),bytes,pInfo->size_);
            v.m_env->CallVoidMethod(gObject,gOnFileProgressCB,percent(bytes,pInfo->size_));
        }else{
            __android_log_print(ANDROID_LOG_ERROR,TAG,"libusb_bulk_transfer=%d",r);
            return false;
        }
    }
}

static void FileInfo(FILEINFO &info,int files,int index,MetaInfo mi)
{
    info.files_ = files;
    info.index_ = index;
    info.name_.assign(mi.name.begin(),mi.name.end());
    info.size_ = mi.size;
}

static void allFilesSent()
{
    JavaVm v(gJavaVM);
    if(v.getEnv(JNI_VERSION_1_4)){
        __android_log_print(ANDROID_LOG_INFO,TAG,"Attached to current thread");
    }else{
        __android_log_print(ANDROID_LOG_ERROR,TAG,"failed to attach to current thread");
        return;
    }

    jstring js = v.m_env->NewStringUTF("Terminating writer thread");
    v.m_env->CallVoidMethod(gObject,gOnAllFilesSentCB,js);
}

static void* writerThread(void *arg) {
    unsigned char ep = *((unsigned char*)arg);
    __android_log_print(ANDROID_LOG_INFO,TAG,"writerThread starts(ep:0x%x)...",ep);
    unsigned char *buf = new unsigned char[BUF_SIZE];

    JavaVm v(gJavaVM);
    if(v.getEnv(JNI_VERSION_1_4)){
        __android_log_print(ANDROID_LOG_INFO,TAG,"Attached to current thread");
    }else{
        __android_log_print(ANDROID_LOG_ERROR,TAG,"failed to attach to current thread");
    }

    gBytes = 0;
    if(gFileList.size()==0) {
        send(ep,buf,BUF_SIZE);
        return ((void*)0);
    }else{
        FILEINFO info;
        for(unsigned int i=0;i<gFileList.size();i++) {
            FileInfo(info,gFileList.size(),i,gFileList.at(i));
            __android_log_print(ANDROID_LOG_INFO,TAG,"Processing [%d/%d]-%s (%d)",i,info.files_,gFileList.at(i).name.c_str(),info.size_);
            jstring js = v.m_env->NewStringUTF((fileOrder(i,info.files_)+std::string("Sending '")+stripPath(gFileList.at(i).name)+std::string("'")).c_str());
            v.m_env->CallVoidMethod(gObject,gOnMessage,js);

            auto start = std::chrono::high_resolution_clock::now();
            if(sendFile(ep,buf,BUF_SIZE,&info,gFileList.at(i))) {
                auto stop = std::chrono::high_resolution_clock::now();
                float sec = std::chrono::duration_cast<std::chrono::milliseconds>(stop-start).count()/1000.;
                jstring js = v.m_env->NewStringUTF((commas(std::to_string(info.size_))+"Bytes "+elapsedTime(stop-start)
                        +" "+Bps(info.size_,sec)+"("+bps(info.size_,sec)+")").c_str());
                v.m_env->CallVoidMethod(gObject,gOnMessage,js);
                __android_log_print(ANDROID_LOG_INFO,TAG,"sec = %f",std::chrono::duration_cast<std::chrono::milliseconds>(stop-start).count()/1000.);
                __android_log_print(ANDROID_LOG_INFO,TAG,"bytes = %d",info.size_);
                __android_log_print(ANDROID_LOG_INFO,TAG,"Bps = %f",(float)info.size_/(float)(std::chrono::duration_cast<std::chrono::milliseconds>(stop-start).count()/1000.));
            }
        }
    }
    delete [] buf;
    allFilesSent();
    return ((void*)0);
}

static void getMethodLog(jmethodID m, const char *funcName)
{
    if(m==0) {
    __android_log_print( ANDROID_LOG_ERROR, TAG, "Can't find the function: %s",funcName ) ;
    }else{
    __android_log_print( ANDROID_LOG_INFO, TAG, "%s Method connection ok",funcName ) ;
    }
}

static void initFuncPointers(JNIEnv *env)
{
    __android_log_print(ANDROID_LOG_INFO, TAG, "FindClass...%s", gClassName);

    jclass cls = findClass(env,gClassName);
    if(cls == NULL){
        __android_log_print(ANDROID_LOG_ERROR, TAG, "Can't find the class, %s", gClassName);
    } else {
        __android_log_print(ANDROID_LOG_INFO, TAG, "Class %s found", gClassName);
    }

    gOnMessage = getMethod(env,cls,"onMessage","(Ljava/lang/String;)V");
    getMethodLog(gOnMessage,"onMessage");

    gOnFileStartRecceivingCB = getMethod(env,cls,"onFileStartReceiving","(Ljava/lang/String;)V");
    getMethodLog(gOnFileStartRecceivingCB,"onFileStartReceiving");

    gOnFileReceivedCB = getMethod(env,cls,"onFileReceived","(Ljava/lang/String;)V");
    getMethodLog(gOnFileReceivedCB,"onFileReceived");

    gOnFileSentCB = getMethod(env,cls,"onFileSent","(Ljava/lang/String;)V");
    getMethodLog(gOnFileSentCB,"onFileSent");

    gOnAllFilesSentCB = getMethod(env,cls,"onAllFilesSent","(Ljava/lang/String;)V");
    getMethodLog(gOnAllFilesSentCB,"onAllFilesSent");

    gOnFileProgressCB  = getMethod(env,cls,"onFileProgress","(I)V");
    getMethodLog(gOnFileProgressCB,"onFileProgress");

    gOnFileName = getMethod(env,cls,"onFileName","(Ljava/lang/String;)I");
    getMethodLog(gOnFileName,"onFileName");
}

static int ResetDevice(libusb_device_handle *devh)
{
    int status = libusb_control_transfer(devh, LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR |
                                               LIBUSB_RECIPIENT_DEVICE, 0x3, 0, 0, (unsigned char*)"FX3 RST", 7,0);
    __android_log_print(ANDROID_LOG_INFO,TAG,"libusb_control_transfer ResetDevice=%d",status);
    return status;
}

static void GetFirmwareVersion(libusb_device_handle *devh)
{
    int status;
    status = libusb_control_transfer(devh, LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR |
                                            +                                           LIBUSB_RECIPIENT_DEVICE, 0x3, 0, 0, (unsigned char*)"VER", 3,100);
    __android_log_print(ANDROID_LOG_INFO,TAG,"libusb_control_transfer VER=%d",status);
    usleep(100000);

    unsigned char buf[6]={0,};
    status = libusb_control_transfer(devh, LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR |
                                            +                                           LIBUSB_RECIPIENT_DEVICE, 0x3, 0, 0, buf, 5,100);
    usleep(100000);

    gFwVersion = reinterpret_cast<char*>(buf);
    __android_log_print(ANDROID_LOG_INFO,TAG,"Firmware version=%s",gFwVersion.c_str());
}

static void GetZingMode(libusb_device_handle *devh)
{
    int status;
    status = libusb_control_transfer(devh, LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR |
                                           LIBUSB_RECIPIENT_DEVICE, 0x3, 0, 0, (unsigned char*)"DMA MODE SYNC", 13,100);
    __android_log_print(ANDROID_LOG_INFO,TAG,"libusb_control_transfer DMA MODE SYC=%d",status);
    usleep(100000);

    status = libusb_control_transfer(devh, LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR |
                                           LIBUSB_RECIPIENT_DEVICE, 0x3, 0, 0, (unsigned char*)"GET ZING MODE", 13,100);
    __android_log_print(ANDROID_LOG_INFO,TAG,"libusb_control_transfer DMA MODE SYC=%d",status);
    usleep(100000);

    unsigned char buf[4]={0,};
    status = libusb_control_transfer(devh, LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR |
                                           LIBUSB_RECIPIENT_DEVICE, 0x3, 0, 0, buf, 3,100);
    __android_log_print(ANDROID_LOG_INFO,TAG,"ZING MODE=%s",buf);
    usleep(100000);

    if(strncmp((char*)buf,"DEV",3)==0)
        gZingMode = 0;
    else if(strncmp((char*)buf,"PPC",3)==0)
        gZingMode = 1;
    else
        gZingMode = -1;

    GetFirmwareVersion(devh);
    status = libusb_control_transfer(devh, LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR |
                                           LIBUSB_RECIPIENT_DEVICE, 0x3, 0, 0, (unsigned char*)"DMA MODE NORMAL", 15,100);
    __android_log_print(ANDROID_LOG_INFO,TAG,"libusb_control_transfer DMA MODE NORMAL=%d",status);
    usleep(100000);
}

static void SetZingMode(libusb_device_handle *devh, int mode)
{
    int status;
    status = libusb_control_transfer(devh, LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR |
                                           LIBUSB_RECIPIENT_DEVICE, 0x3, 0, 0, (unsigned char*)"DMA MODE SYNC", 13,100);
    __android_log_print(ANDROID_LOG_INFO,TAG,"libusb_control_transfer DMA MODE SYC=%d",status);
    usleep(100000);

    if(mode==0) {
        status = libusb_control_transfer(devh, LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR |
                                               LIBUSB_RECIPIENT_DEVICE, 0x3, 0, 0, (unsigned char*)"ZING MODE DEV", 13,100);
        __android_log_print(ANDROID_LOG_INFO,TAG,"libusb_control_transfer ZING MODE DEV=%d",status);
    }else if(mode==1) {
        status = libusb_control_transfer(devh, LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR |
                                               LIBUSB_RECIPIENT_DEVICE, 0x3, 0, 0, (unsigned char*)"ZING MODE PPC", 13,100);
        __android_log_print(ANDROID_LOG_INFO,TAG,"libusb_control_transfer ZING MODE PPC=%d",status);
    }
    usleep(100000);

    status = libusb_control_transfer(devh, LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR |
                                           LIBUSB_RECIPIENT_DEVICE, 0x3, 0, 0, (unsigned char*)"DMA MODE NORMAL", 15,100);
    __android_log_print(ANDROID_LOG_INFO,TAG,"libusb_control_transfer DMA MODE NORMAL=%d",status);
    usleep(100000);
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_example_gstreamer_MainActivity_stringFromJNI(
        JNIEnv* env,
        jobject /* this */) {

    const struct libusb_version* libUsbVersion = libusb_get_version();
    __android_log_print(ANDROID_LOG_INFO,TAG,"libusb v%d.%d.%d.%d",libUsbVersion->major,libUsbVersion->minor,libUsbVersion->micro,libUsbVersion->nano);

    std::string hello = "gStreamer Android v0.2.5";
    return env->NewStringUTF(hello.c_str());
}

extern "C" JNIEXPORT jint JNICALL
Java_com_example_gstreamer_MainActivity_open
        (JNIEnv *env, jobject thiz, jint fileDescriptor)
{
    __android_log_print(ANDROID_LOG_INFO,TAG,"open starts");
    int r;

    r = libusb_set_option(NULL, LIBUSB_OPTION_WEAK_AUTHORITY, NULL);
    if(r<0) {
        __android_log_print(ANDROID_LOG_INFO,TAG,"libusb_set_option error=%d",r);
        return r;
    }

    r = libusb_init(NULL);
    if(r<0) {
        __android_log_print(ANDROID_LOG_INFO,TAG,"libusb_init error=%d",r);
        return r;
    }

    r = libusb_wrap_sys_device(NULL,(intptr_t)fileDescriptor,&gDevh);
    if(r<0) {
        __android_log_print(ANDROID_LOG_INFO,TAG,"libusb_wrap_sys_device error=%d",r);
        return r;
    }

    GetZingMode(gDevh);
    r = deviceInfo(gDevh);
    __android_log_print(ANDROID_LOG_INFO,TAG,"deviceInfo = %d",r);
    if(r<0) return r;

    r = libusb_kernel_driver_active(gDevh, 0);
    __android_log_print(ANDROID_LOG_INFO,TAG,"libusb_kernel_driver_active = %d",r);
    if(r<0) return r;

    gObject = env->NewGlobalRef(thiz);
    initFuncPointers(env);
    memset(&gPrev,0,sizeof(ByteSec));
    return 0;
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_gstreamer_MainActivity_close
        (JNIEnv *env, jobject thiz)
{
    if(gObject) {
        env->DeleteGlobalRef(gObject);
        gObject = NULL;
    }
    libusb_exit(NULL);
}

extern "C" JNIEXPORT jint JNICALL
Java_com_example_gstreamer_MainActivity_reader
        (JNIEnv *env, jobject thiz)
{
    __android_log_print(ANDROID_LOG_INFO,TAG,"reader starts");

    gRcv.run = true;
    return pthread_create(&gRcv.id, NULL, readerThread, &gEpIN);
}

extern "C" JNIEXPORT jint JNICALL
        Java_com_example_gstreamer_MainActivity_stopReader
        (JNIEnv *env, jobject thiz)
{
    __android_log_print(ANDROID_LOG_INFO,TAG,"stopReader starts");

    void *result;
    gRcv.run = false;
    int r = 0;
    if(gRcv.id!=-1) {
        r = pthread_join(gRcv.id,&result);
        gRcv.id = -1;
    }
    return r;
}

extern "C" JNIEXPORT jlong JNICALL
Java_com_example_gstreamer_MainActivity_count
        (JNIEnv *, jobject)
{
    return gCount;
}

extern "C" JNIEXPORT jint JNICALL
Java_com_example_gstreamer_MainActivity_writer
        (JNIEnv *env, jobject thiz, jobject fileList)
{
    __android_log_print(ANDROID_LOG_INFO,TAG,"writer starts");

    gFileList.clear();
    if(fileList!=NULL) {
        jclass java_util_ArrayList = static_cast<jclass>(env->NewGlobalRef(env->FindClass("java/util/ArrayList")));
        jmethodID java_util_ArrayList_size = env->GetMethodID (java_util_ArrayList, "size", "()I");
        jmethodID java_util_ArrayList_get = env->GetMethodID(java_util_ArrayList, "get", "(I)Ljava/lang/Object;");
        jint len = env->CallIntMethod(fileList, java_util_ArrayList_size);

        jclass cMetaInfo = env->FindClass("com/example/gstreamer/MetaInfo");
        jfieldID fName = env->GetFieldID(cMetaInfo,"name","Ljava/lang/String;");
        jfieldID fSize = env->GetFieldID(cMetaInfo,"size", "J");
        jfieldID fFd = env->GetFieldID(cMetaInfo,"fd", "I");

        for(jint i=0;i<len;i++) {
            jobject obj = env->CallObjectMethod(fileList, java_util_ArrayList_get, i);
            jobject objName = env->GetObjectField(obj,fName);
            long size = env->GetLongField(obj,fSize);
            int fd = env->GetIntField(obj,fFd);

            jstring jstr = static_cast<jstring>(objName);
            const char* pchars = env->GetStringUTFChars(jstr, nullptr);

            MetaInfo mi;
            mi.name = pchars;
            mi.fd = fd;
            mi.size = size;
            gFileList.emplace_back(mi);

            env->ReleaseStringUTFChars(jstr, pchars);
            env->DeleteLocalRef(jstr);
        }
        __android_log_print(ANDROID_LOG_INFO,TAG,"FileList size=%lu",gFileList.size());
        for(unsigned int i=0;i<gFileList.size();i++) __android_log_print(ANDROID_LOG_INFO,TAG,"%d-%s",i,gFileList.at(i).name.c_str());
    }
    gSnd.run = true;
    return pthread_create(&gSnd.id,NULL,writerThread,&gEpOut);
}

extern "C" JNIEXPORT jint JNICALL
Java_com_example_gstreamer_MainActivity_stopWriter
        (JNIEnv *env, jobject thiz)
{
    __android_log_print(ANDROID_LOG_INFO,TAG,"stopWriter starts");

    void *result;
    gSnd.run = false;
    int r = 0;
    if(gSnd.id!=-1) {
        r = pthread_join(gSnd.id,&result);
        gSnd.id = -1;
    }
    return r;
}

extern "C" JNIEXPORT jint JNICALL
Java_com_example_gstreamer_MainActivity_bps
        (JNIEnv *, jobject)
{
    if(gPrev.bytes==0) gPrev.bytes = gBytes;

    auto stop = std::chrono::high_resolution_clock::now();
    size_t bytes = gBytes;
    float sec = std::chrono::duration_cast<std::chrono::milliseconds>(stop-gPrev.now).count()/1000.;

    //__android_log_print(ANDROID_LOG_INFO,TAG,"BytesDiff=%d Sec=%f",bytes-gPrev.bytes,sec);

    int bps = 8*(int)BpsVal(bytes-gPrev.bytes,sec);

    gPrev.now = stop;
    gPrev.bytes = bytes;
    return bps;
}

extern "C" JNIEXPORT jint JNICALL
Java_com_example_gstreamer_MainActivity_zingMode
        (JNIEnv *, jobject)
{
    return gZingMode;
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_example_gstreamer_MainActivity_firmwareVer
        (JNIEnv *env, jobject)
{
    return env->NewStringUTF(gFwVersion.c_str());
}

extern "C" JNIEXPORT jint JNICALL
Java_com_example_gstreamer_MainActivity_setZingMode
        (JNIEnv *env, jobject thiz, jint mode)
{
    if(gDevh) {
        SetZingMode(gDevh,mode);
        return 0;
    }
    return -1;
}

extern "C" jint
JNI_OnLoad(JavaVM* vm, void* reserved)
{
    __android_log_print(ANDROID_LOG_INFO,TAG,"JNI_OnLoad start");
    JNIEnv* env = NULL;
    if(vm->GetEnv((void**)&env,JNI_VERSION_1_6)!=JNI_OK){
        __android_log_print(ANDROID_LOG_ERROR,TAG,"JNI_OnLoad GetEnv Error");
        return JNI_ERR;
    }
    gJavaVM = vm;
    __android_log_print(ANDROID_LOG_INFO,TAG,"JNI_OnLoad end, JNIEnv=0x%p",env);
    return JNI_VERSION_1_6;
}

extern "C" void
JNI_OnUnload(JavaVM* vm, void* reserved)
{
    __android_log_print(ANDROID_LOG_INFO,TAG,"JNI_OnUnload start");
    __android_log_print(ANDROID_LOG_INFO,TAG,"JNI_OnUnload end");
}