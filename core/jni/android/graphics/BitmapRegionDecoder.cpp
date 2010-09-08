/*
 * Copyright (C) 2010 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "BitmapRegionDecoder"

#include "SkBitmap.h"
#include "SkImageEncoder.h"
#include "GraphicsJNI.h"
#include "SkUtils.h"
#include "SkTemplates.h"
#include "SkPixelRef.h"
#include "SkStream.h"
#include "BitmapFactory.h"
#include "AutoDecodeCancel.h"
#include "SkBitmapRegionDecoder.h"
#include "CreateJavaOutputStreamAdaptor.h"
#include "Utils.h"

#include <android_runtime/AndroidRuntime.h>
#include "android_util_Binder.h"
#include "android_nio_utils.h"
#include "CreateJavaOutputStreamAdaptor.h"

#include <binder/Parcel.h>
#include <jni.h>
#include <utils/Asset.h>

static jclass gFileDescriptor_class;
static jfieldID gFileDescriptor_descriptor;

#if 0
    #define TRACE_BITMAP(code)  code
#else
    #define TRACE_BITMAP(code)
#endif

using namespace android;

static jobject doBuildTileIndex(JNIEnv* env, SkStream* stream, bool isShareable) {
    SkImageDecoder* decoder = SkImageDecoder::Factory(stream);
    int width, height;
    if (NULL == decoder) {
        doThrowIOE(env, "Image format not supported");
        return nullObjectReturn("SkImageDecoder::Factory returned null");
    }

    JavaPixelAllocator *javaAllocator = new JavaPixelAllocator(env, true);
    decoder->setAllocator(javaAllocator);
    JavaMemoryUsageReporter *javaMemoryReporter = new JavaMemoryUsageReporter(env);
    decoder->setReporter(javaMemoryReporter);
    javaAllocator->unref();
    javaMemoryReporter->unref();

    if (!decoder->buildTileIndex(stream, &width, &height, isShareable)) {
        char msg[1024];
        snprintf(msg, 1023, "Image failed to decode using %s decoder", decoder->getFormatName());
        doThrowIOE(env, msg);
        return nullObjectReturn("decoder->buildTileIndex returned false");
    }

    SkBitmapRegionDecoder *brd = new SkBitmapRegionDecoder(decoder, width, height);

    return GraphicsJNI::createBitmapRegionDecoder(env, brd);
}

static jobject nativeNewInstanceFromByteArray(JNIEnv* env,
        jobject, jbyteArray byteArray, int offset, int length,
        jboolean isShareable) {
    AutoJavaByteArray ar(env, byteArray);
    SkStream* stream = new SkMemoryStream(ar.ptr() + offset, length, false);
    SkAutoUnref aur(stream);
    if (isShareable) {
        aur.detach();
    }
    return doBuildTileIndex(env, stream, isShareable);
}

static jobject nativeNewInstanceFromFileDescriptor(JNIEnv* env,
        jobject clazz, jobject fileDescriptor, jboolean isShareable) {
    NPE_CHECK_RETURN_ZERO(env, fileDescriptor);

    jint descriptor = env->GetIntField(fileDescriptor,
                                       gFileDescriptor_descriptor);
    bool weOwnTheFD = false;

    if (isShareable) {
        int newFD = ::dup(descriptor);
        if (-1 != newFD) {
            weOwnTheFD = true;
            descriptor = newFD;
        }
    }

    SkFDStream* stream = new SkFDStream(descriptor, weOwnTheFD);
    SkAutoUnref aur(stream);
    if (!stream->isValid()) {
        return NULL;
    }

    if (isShareable) {
        aur.detach();
    }

    // Restore our offset when we leave, so we can be called more than once
    // with the same descriptor. This is only required if we didn't dup the
    // file descriptor, but it is OK to do it all the time.
    AutoFDSeek as(descriptor);

    return doBuildTileIndex(env, stream, isShareable);
}

static jobject nativeNewInstanceFromStream(JNIEnv* env, jobject clazz,
                                  jobject is,       // InputStream
                                  jbyteArray storage, // byte[]
                                  jboolean isShareable) {
    jobject BitmapRegionDecoder = NULL;
    SkStream* stream = CreateJavaInputStreamAdaptor(env, is, storage, 1024);

    if (stream) {
        // for now we don't allow shareable with java inputstreams
        BitmapRegionDecoder = doBuildTileIndex(env, stream, false);
        stream->unref();
    }
    return BitmapRegionDecoder;
}

static jobject nativeNewInstanceFromAsset(JNIEnv* env, jobject clazz,
                                 jint native_asset, // Asset
                                 jboolean isShareable) {
    SkStream* stream;
    Asset* asset = reinterpret_cast<Asset*>(native_asset);
    stream = new AssetStreamAdaptor(asset);
    SkAutoUnref aur(stream);
    if (isShareable) {
        aur.detach();
    }
    return doBuildTileIndex(env, stream, isShareable);
}

/*
 * nine patch not supported
 *
 * purgeable not supported
 * reportSizeToVM not supported
 */
static jobject nativeDecodeRegion(JNIEnv* env, jobject, SkBitmapRegionDecoder *brd,
        int start_x, int start_y, int width, int height, jobject options) {
    SkImageDecoder *decoder = brd->getDecoder();
    int sampleSize = 1;
    SkBitmap::Config prefConfig = SkBitmap::kNo_Config;
    bool doDither = true;

    if (NULL != options) {
        sampleSize = env->GetIntField(options, gOptions_sampleSizeFieldID);
        // initialize these, in case we fail later on
        env->SetIntField(options, gOptions_widthFieldID, -1);
        env->SetIntField(options, gOptions_heightFieldID, -1);
        env->SetObjectField(options, gOptions_mimeFieldID, 0);

        jobject jconfig = env->GetObjectField(options, gOptions_configFieldID);
        prefConfig = GraphicsJNI::getNativeBitmapConfig(env, jconfig);
        doDither = env->GetBooleanField(options, gOptions_ditherFieldID);
    }

    decoder->setDitherImage(doDither);
    SkBitmap*           bitmap = new SkBitmap;
    SkAutoTDelete<SkBitmap>       adb(bitmap);
    AutoDecoderCancel   adc(options, decoder);

    // To fix the race condition in case "requestCancelDecode"
    // happens earlier than AutoDecoderCancel object is added
    // to the gAutoDecoderCancelMutex linked list.
    if (NULL != options && env->GetBooleanField(options, gOptions_mCancelID)) {
        return nullObjectReturn("gOptions_mCancelID");;
    }

    SkIRect region;
    region.fLeft = start_x;
    region.fTop = start_y;
    region.fRight = start_x + width;
    region.fBottom = start_y + height;

    if (!brd->decodeRegion(bitmap, region, prefConfig, sampleSize)) {
        return nullObjectReturn("decoder->decodeRegion returned false");
    }

    // update options (if any)
    if (NULL != options) {
        env->SetIntField(options, gOptions_widthFieldID, bitmap->width());
        env->SetIntField(options, gOptions_heightFieldID, bitmap->height());
        // TODO: set the mimeType field with the data from the codec.
        // but how to reuse a set of strings, rather than allocating new one
        // each time?
        env->SetObjectField(options, gOptions_mimeFieldID,
                            getMimeTypeString(env, decoder->getFormat()));
    }

    // detach bitmap from its autotdeleter, since we want to own it now
    adb.detach();

    SkPixelRef* pr;
    pr = bitmap->pixelRef();
    // promise we will never change our pixels (great for sharing and pictures)
    pr->setImmutable();
    // now create the java bitmap
    return GraphicsJNI::createBitmap(env, bitmap, false, NULL);
}

static int nativeGetHeight(JNIEnv* env, jobject, SkBitmapRegionDecoder *brd) {
    return brd->getHeight();
}

static int nativeGetWidth(JNIEnv* env, jobject, SkBitmapRegionDecoder *brd) {
    return brd->getWidth();
}

static void nativeClean(JNIEnv* env, jobject, SkBitmapRegionDecoder *brd) {
    delete brd;
}

///////////////////////////////////////////////////////////////////////////////

#include <android_runtime/AndroidRuntime.h>

static JNINativeMethod gBitmapRegionDecoderMethods[] = {
    {   "nativeDecodeRegion",
        "(IIIIILandroid/graphics/BitmapFactory$Options;)Landroid/graphics/Bitmap;",
        (void*)nativeDecodeRegion},

    {   "nativeGetHeight", "(I)I", (void*)nativeGetHeight},

    {   "nativeGetWidth", "(I)I", (void*)nativeGetWidth},

    {   "nativeClean", "(I)V", (void*)nativeClean},

    {   "nativeNewInstance",
        "([BIIZ)Landroid/graphics/BitmapRegionDecoder;",
        (void*)nativeNewInstanceFromByteArray
    },

    {   "nativeNewInstance",
        "(Ljava/io/InputStream;[BZ)Landroid/graphics/BitmapRegionDecoder;",
        (void*)nativeNewInstanceFromStream
    },

    {   "nativeNewInstance",
        "(Ljava/io/FileDescriptor;Z)Landroid/graphics/BitmapRegionDecoder;",
        (void*)nativeNewInstanceFromFileDescriptor
    },

    {   "nativeNewInstance",
        "(IZ)Landroid/graphics/BitmapRegionDecoder;",
        (void*)nativeNewInstanceFromAsset
    },
};

#define kClassPathName  "android/graphics/BitmapRegionDecoder"

int register_android_graphics_BitmapRegionDecoder(JNIEnv* env);
int register_android_graphics_BitmapRegionDecoder(JNIEnv* env)
{
    return android::AndroidRuntime::registerNativeMethods(env, kClassPathName,
            gBitmapRegionDecoderMethods, SK_ARRAY_COUNT(gBitmapRegionDecoderMethods));
}