//
// Created by rossi on 21. 5. 6.
//

#ifndef GSTREAMER_JAVAVM_H
#define GSTREAMER_JAVAVM_H

#include "../../../../../../Android/Sdk/ndk/21.1.6352462/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/include/jni.h"
#include "../../../../../../Android/Sdk/ndk/21.1.6352462/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/include/android/log.h"

#define TAG "glsusb"

class JavaVm {
    JavaVm();
public:
    JavaVm(JavaVM *vm):m_JavaVM(vm),m_env(0),m_isAttached(false){}
    ~JavaVm(){ if(m_isAttached) m_JavaVM->DetachCurrentThread(); }

    bool getEnv(int jniVersion){
        if(!m_JavaVM) return false;
        int status = m_JavaVM->GetEnv((void **) &m_env, jniVersion);
        if(status < JNI_OK){
            __android_log_print(ANDROID_LOG_INFO,TAG,"getEnv: failed to get JNI environment, assuming native thread, %d",status);
            status = m_JavaVM->AttachCurrentThread(&m_env, 0);
            if(status < JNI_OK) {
                __android_log_print(ANDROID_LOG_ERROR,TAG,"getEnv: failed to attach current thread, %d",status);
                return false;
            }
            __android_log_print(ANDROID_LOG_INFO,TAG,"getEnv: Attached to current thread, %d",status);
            m_isAttached = true;
        }
        return true;
    }

    JavaVM *m_JavaVM;
    JNIEnv *m_env;
    bool m_isAttached;
};


#endif //GSTREAMER_JAVAVM_H
