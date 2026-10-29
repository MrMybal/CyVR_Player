#include "cyvr/player/android/bridge.hpp"
#include <android_native_app_glue.h>
#include <unistd.h>

namespace cyvr::player {
namespace {
struct Env {
    JavaVM *vm;
    JNIEnv *env{};
    bool attached{};
    explicit Env(JavaVM *v) : vm(v) {
        if (vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) != JNI_OK) {
            if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK)
                throw MediaError("JNI attach failed");
            attached = true;
        }
    }
    ~Env() {
        if (attached)
            vm->DetachCurrentThread();
    }
    void check() {
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            throw MediaError("Android denied the requested operation.");
        }
    }
};
std::string string(JNIEnv *env, jstring value) {
    if (!value)
        return {};
    const char *chars = env->GetStringUTFChars(value, nullptr);
    if (!chars)
        throw MediaError("JNI string allocation failed");
    std::string result(chars);
    env->ReleaseStringUTFChars(value, chars);
    env->DeleteLocalRef(value);
    return result;
}
std::vector<std::string> strings(JNIEnv *env, jobjectArray values) {
    std::vector<std::string> out;
    if (!values)
        return out;
    const auto size = env->GetArrayLength(values);
    for (int i = 0; i < size; ++i)
        out.push_back(string(env, static_cast<jstring>(env->GetObjectArrayElement(values, i))));
    env->DeleteLocalRef(values);
    return out;
}
} // namespace
AndroidBridge::AndroidBridge(android_app *app)
    : vm_(app->activity->vm), root_(app->activity->externalDataPath ? app->activity->externalDataPath
                                                                    : app->activity->internalDataPath),
      state_file_(std::string(app->activity->internalDataPath)+"/player-state.bin") {
    Env e(vm_);
    activity_ = e.env->NewGlobalRef(app->activity->clazz);
    auto type = e.env->GetObjectClass(activity_);
    type_ = static_cast<jclass>(e.env->NewGlobalRef(type));
    e.env->DeleteLocalRef(type);
    tree_ = e.env->GetMethodID(type_, "requestTree", "()V");
    poll_ = e.env->GetMethodID(type_, "pollEvent", "()[Ljava/lang/String;");
    saved_ = e.env->GetMethodID(type_, "savedTree", "()Ljava/lang/String;");
    certs_ = e.env->GetMethodID(type_, "exportCertificates", "()Ljava/lang/String;");
    codec_ = e.env->GetMethodID(type_, "hardwareCodec", "(Ljava/lang/String;II)Ljava/lang/String;");
    list_ = e.env->GetMethodID(type_, "listDocuments", "(Ljava/lang/String;)[Ljava/lang/String;");
    open_ = e.env->GetMethodID(type_, "openDocument", "(Ljava/lang/String;)[J");
    close_ = e.env->GetMethodID(type_, "closeDocument", "(I)V");
    cancel_ = e.env->GetMethodID(type_, "cancelQuery", "()V");
    e.check();
}
AndroidBridge::~AndroidBridge() {
    Env e(vm_);
    e.env->DeleteGlobalRef(type_);
    e.env->DeleteGlobalRef(activity_);
}
Platform AndroidBridge::platform() {
    Platform p;
    p.root = root_;
    p.state_file=state_file_;
    // Certificate extraction is initialized before the XR loop, never in a frame.
    Env e(vm_);
    auto pem = static_cast<jstring>(e.env->CallObjectMethod(activity_, certs_));
    e.check();
    p.ca_file = string(e.env, pem);
    p.request_tree = [this] {
        Env e(vm_);
        e.env->CallVoidMethod(activity_, tree_);
        e.check();
    };
    p.cancel_documents = [this] {
        Env e(vm_);
        e.env->CallVoidMethod(activity_, cancel_);
        e.check();
    };
    p.hardware_codec = [this](const std::string &mime, int w, int h) {
        Env e(vm_);
        auto text = e.env->NewStringUTF(mime.c_str());
        auto name = static_cast<jstring>(e.env->CallObjectMethod(activity_, codec_, text, w, h));
        e.env->DeleteLocalRef(text);
        e.check();
        return string(e.env, name);
    };
    p.documents = [this](const std::string &uri) {
        Env e(vm_);
        auto text = e.env->NewStringUTF(uri.c_str());
        auto array = static_cast<jobjectArray>(e.env->CallObjectMethod(activity_, list_, text));
        e.env->DeleteLocalRef(text);
        e.check();
        auto flat = strings(e.env, array);
        std::vector<BrowserItem> items;
        for (size_t i = 0; i + 3 < flat.size(); i += 4)
            items.push_back({flat[i], flat[i + 1], flat[i + 2] == "1", std::stoull(flat[i + 3])});
        return items;
    };
    p.open_document = [this](const std::string &uri) {
        Env e(vm_);
        auto text = e.env->NewStringUTF(uri.c_str());
        auto values = static_cast<jlongArray>(e.env->CallObjectMethod(activity_, open_, text));
        e.env->DeleteLocalRef(text);
        e.check();
        if (!values || e.env->GetArrayLength(values) != 3)
            throw MediaError("Invalid document descriptor");
        jlong v[3];
        e.env->GetLongArrayRegion(values, 0, 3, v);
        e.env->DeleteLocalRef(values);
        int fd = ::dup(int(v[0]));
        e.env->CallVoidMethod(activity_, close_, int(v[0]));
        e.check();
        if (fd < 0)
            throw MediaError("Cannot duplicate document descriptor");
        return MediaSource::descriptor(fd, v[1], v[2]);
    };
    return p;
}
std::vector<std::string> AndroidBridge::poll() {
    Env e(vm_);
    auto array = static_cast<jobjectArray>(e.env->CallObjectMethod(activity_, poll_));
    e.check();
    return strings(e.env, array);
}
std::string AndroidBridge::saved_tree() {
    Env e(vm_);
    auto value = static_cast<jstring>(e.env->CallObjectMethod(activity_, saved_));
    e.check();
    return string(e.env, value);
}
int AndroidBridge::presentation_mode(){
    Env e(vm_);auto method=e.env->GetMethodID(type_,"presentationMode","()I");e.check();
    const int mode=e.env->CallIntMethod(activity_,method);e.check();return mode;
}
} // namespace cyvr::player
