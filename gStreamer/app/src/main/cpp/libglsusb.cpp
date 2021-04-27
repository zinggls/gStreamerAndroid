#include <jni.h>
#include <string>
#include <libusbi.h>
#include <android/log.h>
#include "fileinfo.h"
#include <errno.h>

#define TAG "glsusb"
#define BUF_SIZE    (8192*8*4)

typedef enum{
    NOT_DEF=-1,
    STREAM_MODE,
    FILE_MODE
} Mode;

static libusb_device_handle *gDevh = NULL;
static unsigned int gCount;
static unsigned char gEpIN = 0x82;   //Input EP
static unsigned char gEpOut = 0x02;   //Output EP
static unsigned char gSync[4] = {0x07, 0x3a, 0xb6, 0x99 };
static Mode gMode = NOT_DEF;
static const char *gClassName = "com/example/gstreamer/MainActivity";
static jclass gClass = NULL;
static jmethodID gStaticCB = NULL;
static JavaVM *gJavaVM = NULL;
static jmethodID gOnFileReceivedCB = NULL;

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

static int convertNameSizeFromUnicodeToAscii(unsigned char *buffer)
{
    int unicodeSize;
    memcpy(&unicodeSize, buffer, sizeof(int));
    return unicodeSize/2;
}

static int getFileInfo(unsigned char *buffer, int bufferSize, int syncSize, FILEINFO &info)
{
    int nOffset = syncSize;
    memset(info.name_, 0, sizeof(FILEINFO::name_));
    memcpy(&info.index_, buffer + nOffset, sizeof(int)); nOffset += sizeof(int);
    memcpy(&info.files_, buffer + nOffset, sizeof(int)); nOffset += sizeof(int);

    info.nameSize_ = convertNameSizeFromUnicodeToAscii(buffer + nOffset); nOffset += sizeof(int);
    convertNameFromUnicodeToAscii(buffer + nOffset, 2*info.nameSize_, info.name_); nOffset += 2*info.nameSize_;

    memcpy(&info.size_, buffer + nOffset, sizeof(unsigned int)); nOffset += sizeof(unsigned int);

    assert(nOffset <= bufferSize);	//len보다 작거나 같다는 가정
    return nOffset;
}

static void onFileClose(FILE *pFile)
{
    int status;
    JNIEnv *env;
    bool isAttached = false;

    fclose(pFile);

    assert(gJavaVM);
    status = gJavaVM->GetEnv((void **) &env, JNI_VERSION_1_4);
    if(status < JNI_OK){
        assert(status==JNI_EDETACHED);
        __android_log_print(ANDROID_LOG_INFO,TAG,"onFileClose: failed to get JNI environment, assuming native thread, %d",status);
        status = gJavaVM->AttachCurrentThread(&env, NULL);
        if(status < JNI_OK) {
            __android_log_print(ANDROID_LOG_ERROR,TAG,"onFileClose: failed to attach current thread, %d",status);
            return;
        }
        __android_log_print(ANDROID_LOG_INFO,TAG,"onFileClose: Attached to current thread, %d",status);
        isAttached = true;
    }

    env->CallStaticVoidMethod(gClass, gStaticCB);

    if(isAttached) gJavaVM->DetachCurrentThread();
}

static void* runThread(void *arg)
{
    int r;
    int transferred = 0;
    unsigned char ep = *((unsigned char*)arg);

    unsigned char *buf = new unsigned char[BUF_SIZE];

    libusb_clear_halt(gDevh, ep);
    __android_log_print(ANDROID_LOG_INFO,TAG,"runThread starts(ep:0x%x)...",ep);
    memset(buf,'\0',BUF_SIZE);
    gCount = 0;

    size_t bytes;
    FILEINFO info;
    FILE *pFile = 0;
    while(1){
        r = libusb_bulk_transfer(gDevh, ep, buf, sizeof(unsigned char) * BUF_SIZE, &transferred, 0);
        if(r==0){
            __android_log_print(ANDROID_LOG_INFO, TAG, "%u %dbytes", ++gCount, transferred);
            if(isInputEP(ep)&&syncFound(buf,sizeof(gSync))) {
                __android_log_print(ANDROID_LOG_INFO,TAG,"InputEP(0x%x) Sync found",ep);

                bytes = 0;
                memset(&info,0,sizeof(info));
                getFileInfo(buf, transferred, sizeof(gSync), info);
                __android_log_print(ANDROID_LOG_INFO,TAG,"index:%d",info.index_);
                __android_log_print(ANDROID_LOG_INFO,TAG,"files:%d",info.files_);
                __android_log_print(ANDROID_LOG_INFO,TAG,"nameSize:%d",info.nameSize_);
                __android_log_print(ANDROID_LOG_INFO,TAG,"%s",info.name_);
                __android_log_print(ANDROID_LOG_INFO,TAG,"size:%u",info.size_);

                char path[512];
                sprintf(path,"/sdcard/download/%s",info.name_);
                __android_log_print(ANDROID_LOG_INFO,TAG,"file path:%s",path);
                pFile = fopen(path,"w");
                if(pFile) {
                    __android_log_print(ANDROID_LOG_INFO, TAG, "fopen(%s) ok", path);
                }else
                    __android_log_print(ANDROID_LOG_INFO,TAG,"fopen(%s) failed, error=%s",path,strerror(errno));
            }else{
                if(pFile) {
                    __android_log_print(ANDROID_LOG_INFO,TAG,"bytes: %zu received: %d",bytes,transferred);
                    if(bytes+transferred <= info.size_) {
                        size_t szWrite = fwrite(buf,1,transferred,pFile);
                        __android_log_print(ANDROID_LOG_INFO,TAG,"bytes: %zu written to file",szWrite);
                        assert(szWrite==transferred);
                        bytes += transferred;
                        if(bytes == info.size_) onFileClose(pFile);
                    }else if(bytes+transferred > info.size_) {
                        size_t szWrite = fwrite(buf,1,info.size_-bytes,pFile);
                        assert(szWrite==(info.size_-bytes));
                        __android_log_print(ANDROID_LOG_INFO,TAG,"bytes: %zu written to file",szWrite);
                        bytes += (info.size_-bytes);
                        assert(bytes==info.size_);
                        onFileClose(pFile);
                    }
                    __android_log_print(ANDROID_LOG_INFO,TAG,"file:%s bytes/Total= %zu/%u",info.name_,bytes,info.size_);
                }
            }
        }else{
            delete [] buf;
            __android_log_print(ANDROID_LOG_ERROR,TAG,"libusb_bulk_transfer=%d",r);
            goto cleanup;
        }
    }

cleanup:
    return NULL;
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_example_gstreamer_MainActivity_stringFromJNI(
        JNIEnv* env,
        jobject /* this */) {

    std::string hello = "Hello from C++";
    return env->NewStringUTF(hello.c_str());
}

extern "C" JNIEXPORT jint JNICALL
Java_com_example_gstreamer_MainActivity_open
        (JNIEnv *, jobject, jint fileDescriptor)
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

    r = deviceInfo(gDevh);
    __android_log_print(ANDROID_LOG_INFO,TAG,"deviceInfo = %d",r);
    if(r<0) return r;

    r = libusb_kernel_driver_active(gDevh, 0);
    __android_log_print(ANDROID_LOG_INFO,TAG,"libusb_kernel_driver_active = %d",r);
    if(r<0) return r;

    return 0;
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_gstreamer_MainActivity_close
        (JNIEnv *, jobject)
{
    libusb_exit(NULL);
}

extern "C" JNIEXPORT jint JNICALL
Java_com_example_gstreamer_MainActivity_reader
        (JNIEnv *env, jobject)
{
    __android_log_print(ANDROID_LOG_INFO,TAG,"reader starts");

    __android_log_print(ANDROID_LOG_INFO, TAG, "FindClass...%s", gClassName);
    jclass cls = env->FindClass(gClassName);
    if(cls == NULL){
        __android_log_print(ANDROID_LOG_ERROR, TAG, "Can't find the class, %s", gClassName);
    } else {
        __android_log_print(ANDROID_LOG_INFO, TAG, "Class %s found", gClassName);
    }

    gClass = (jclass)env->NewGlobalRef(cls);
    gStaticCB = env->GetStaticMethodID(cls, "callback", "()V");
    if (gStaticCB == 0 ) {
        __android_log_print( ANDROID_LOG_INFO, TAG, "Can't find the function" ) ;
        env->DeleteGlobalRef(gClass ) ;
    } else {
        __android_log_print( ANDROID_LOG_INFO, TAG, "Method connection ok") ;
        env->CallStaticVoidMethod(cls, gStaticCB );
    }

    gOnFileReceivedCB = env->GetMethodID(cls,"onFileReceived","()V");
    if(gOnFileReceivedCB==0) {
        __android_log_print( ANDROID_LOG_ERROR, TAG, "Can't find the function: %s","onFileReceived" ) ;
    }else{
        __android_log_print( ANDROID_LOG_INFO, TAG, "%s Method connection ok","onFileReceived" ) ;
    }

    pthread_t tid;
    return pthread_create(&tid,NULL,runThread,&gEpIN);
}

extern "C" JNIEXPORT jlong JNICALL
Java_com_example_gstreamer_MainActivity_count
        (JNIEnv *, jobject)
{
    return gCount;
}

extern "C" JNIEXPORT jint JNICALL
Java_com_example_gstreamer_MainActivity_writer
        (JNIEnv *, jobject)
{
    __android_log_print(ANDROID_LOG_INFO,TAG,"writer starts");
    pthread_t tid;
    return pthread_create(&tid,NULL,runThread,&gEpOut);
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

    __android_log_print(ANDROID_LOG_INFO, TAG, "FindClass...%s", gClassName);
    jclass cls = env->FindClass(gClassName);
    if(cls == NULL) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "Can't find the class, %s", gClassName);
        return JNI_ERR;
    }
    __android_log_print(ANDROID_LOG_INFO, TAG, "Class %s found", gClassName);
    __android_log_print(ANDROID_LOG_INFO,TAG,"JNI_OnLoad end, JNIEnv=0x%x",env);
    return JNI_VERSION_1_6;
}

extern "C" void
JNI_OnUnload(JavaVM* vm, void* reserved)
{
    __android_log_print(ANDROID_LOG_INFO,TAG,"JNI_OnUnload start");
    __android_log_print(ANDROID_LOG_INFO,TAG,"JNI_OnUnload end");
}