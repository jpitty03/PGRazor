// PGRazor Zygisk loader.
//
// Runs only in the game's main process. Before specialization it reads the
// PGRazor dex and payload from the module directory (readable only then);
// after specialization, as the app UID and before ActivityThread.main, it
// loads the dex from memory and hands both blobs to dev.pgrazor.RootedEntry.
// Every other process unloads this library immediately. The loader installs
// no hooks itself and keeps no state after postAppSpecialize returns.
#include <android/log.h>
#include <fcntl.h>
#include <jni.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "zygisk.hpp"

#define TAG "PGRazor-zygisk"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

namespace {

constexpr const char *kPackage = "com.nianticlabs.pokemongo";
constexpr size_t kMaxBlob = 256u << 20;

struct Blob {
    unsigned char *data = nullptr;
    size_t size = 0;
    void reset() { free(data); data = nullptr; size = 0; }
};

bool read_at(int dir, const char *path, Blob &out) {
    int fd = openat(dir, path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    struct stat st{};
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
        static_cast<size_t>(st.st_size) > kMaxBlob) {
        close(fd);
        return false;
    }
    out.size = static_cast<size_t>(st.st_size);
    out.data = static_cast<unsigned char *>(malloc(out.size));
    size_t got = 0;
    while (out.data && got < out.size) {
        ssize_t n = read(fd, out.data + got, out.size - got);
        if (n <= 0) break;
        got += static_cast<size_t>(n);
    }
    close(fd);
    if (!out.data || got != out.size) { out.reset(); return false; }
    return true;
}

bool copy_string(JNIEnv *env, jstring value, char *out, size_t size) {
    if (!value) return false;
    const char *chars = env->GetStringUTFChars(value, nullptr);
    if (!chars) return false;
    bool fits = strlen(chars) < size;
    if (fits) strcpy(out, chars);
    env->ReleaseStringUTFChars(value, chars);
    return fits;
}

// Logs a pending Java exception with its stack trace (ExceptionDescribe's stderr
// is not routed to logcat this early) and clears it.
bool failed(JNIEnv *env, const char *step) {
    if (!env->ExceptionCheck()) return false;
    jthrowable error = env->ExceptionOccurred();
    env->ExceptionClear();
    jclass log = env->FindClass("android/util/Log");
    jmethodID trace = log ? env->GetStaticMethodID(log, "getStackTraceString", "(Ljava/lang/Throwable;)Ljava/lang/String;") : nullptr;
    jstring text = trace ? static_cast<jstring>(env->CallStaticObjectMethod(log, trace, error)) : nullptr;
    const char *chars = text && !env->ExceptionCheck() ? env->GetStringUTFChars(text, nullptr) : nullptr;
    LOGE("%s threw: %s", step, chars ? chars : "(no detail)");
    if (chars) env->ReleaseStringUTFChars(text, chars);
    env->ExceptionClear();
    if (text) env->DeleteLocalRef(text);
    if (log) env->DeleteLocalRef(log);
    if (error) env->DeleteLocalRef(error);
    return true;
}

class Loader : public zygisk::ModuleBase {
public:
    void onLoad(zygisk::Api *api, JNIEnv *env) override { api_ = api; env_ = env; }

    void preAppSpecialize(zygisk::AppSpecializeArgs *args) override {
        char name[256] = {};
        target_ = copy_string(env_, args->nice_name, name, sizeof name) && strcmp(name, kPackage) == 0;
        if (!target_) { api_->setOption(zygisk::DLCLOSE_MODULE_LIBRARY); return; }
        if (!copy_string(env_, args->app_data_dir, data_dir_, sizeof data_dir_)) {
            LOGE("no app data directory");
            target_ = false;
            return;
        }
        int dir = api_->getModuleDir();
        if (dir < 0 || !read_at(dir, "pgrazor/classes.dex", dex_) ||
            !read_at(dir, "pgrazor/payload.zip", payload_)) {
            LOGE("module payload missing or unreadable");
            dex_.reset();
            payload_.reset();
            target_ = false;
        }
    }

    void postAppSpecialize(const zygisk::AppSpecializeArgs *) override {
        if (target_) start();
        dex_.reset();
        payload_.reset();
        api_->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
    }

private:
    jbyteArray bytes(const Blob &blob) {
        jbyteArray array = env_->NewByteArray(static_cast<jsize>(blob.size));
        if (array) env_->SetByteArrayRegion(array, 0, static_cast<jsize>(blob.size),
                                            reinterpret_cast<const jbyte *>(blob.data));
        return array;
    }

    void start() {
        JNIEnv *env = env_;
        if (env->PushLocalFrame(32) != 0) { failed(env, "PushLocalFrame"); return; }
        jbyteArray dex = bytes(dex_);
        jbyteArray payload = bytes(payload_);
        if (failed(env, "copy payload") || !dex || !payload) { env->PopLocalFrame(nullptr); return; }

        jclass buffers = env->FindClass("java/nio/ByteBuffer");
        jmethodID wrap = buffers ? env->GetStaticMethodID(buffers, "wrap", "([B)Ljava/nio/ByteBuffer;") : nullptr;
        jclass loaders = env->FindClass("java/lang/ClassLoader");
        jmethodID system = loaders ? env->GetStaticMethodID(loaders, "getSystemClassLoader", "()Ljava/lang/ClassLoader;") : nullptr;
        jmethodID load = loaders ? env->GetMethodID(loaders, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;") : nullptr;
        jclass memory = env->FindClass("dalvik/system/InMemoryDexClassLoader");
        jmethodID create = memory ? env->GetMethodID(memory, "<init>", "(Ljava/nio/ByteBuffer;Ljava/lang/ClassLoader;)V") : nullptr;
        if (failed(env, "resolve loader API") || !wrap || !system || !load || !create) { env->PopLocalFrame(nullptr); return; }

        jobject buffer = env->CallStaticObjectMethod(buffers, wrap, dex);
        jobject parent = env->CallStaticObjectMethod(loaders, system);
        jobject loader = buffer && parent ? env->NewObject(memory, create, buffer, parent) : nullptr;
        if (failed(env, "create dex loader") || !loader) { env->PopLocalFrame(nullptr); return; }

        jstring entry_name = env->NewStringUTF("dev.pgrazor.RootedEntry");
        auto entry = static_cast<jclass>(env->CallObjectMethod(loader, load, entry_name));
        jmethodID entry_start = entry ? env->GetStaticMethodID(entry, "start", "([B[BLjava/lang/String;)V") : nullptr;
        if (failed(env, "load RootedEntry") || !entry_start) { env->PopLocalFrame(nullptr); return; }

        jstring data_dir = env->NewStringUTF(data_dir_);
        env->CallStaticVoidMethod(entry, entry_start, dex, payload, data_dir);
        if (!failed(env, "RootedEntry.start")) LOGI("PGRazor handed off (dex %zu bytes, payload %zu bytes)", dex_.size, payload_.size);
        env->PopLocalFrame(nullptr);
    }

    zygisk::Api *api_ = nullptr;
    JNIEnv *env_ = nullptr;
    bool target_ = false;
    char data_dir_[512] = {};
    Blob dex_, payload_;
};

}  // namespace

REGISTER_ZYGISK_MODULE(Loader)
