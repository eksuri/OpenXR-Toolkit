// MIT License
//
// Copyright(c) 2021 Matthieu Bucchianeri
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this softwareand associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and /or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
// The above copyright noticeand this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "pch.h"

#include <layer.h>

#include "dispatch.h"
#include "factories.h"
#include "log.h"

#ifndef LAYER_NAMESPACE
#error Must define LAYER_NAMESPACE
#endif

using namespace LAYER_NAMESPACE::log;

namespace LAYER_NAMESPACE {

    PFN_xrGetInstanceProcAddr g_bypass = nullptr;

    // Entry point for creating the layer.
    XrResult xrCreateApiLayerInstance(const XrInstanceCreateInfo* const instanceCreateInfo,
                                      const struct XrApiLayerCreateInfo* const apiLayerInfo,
                                      XrInstance* const instance) {
        TraceLocalActivity(local);
        TraceLoggingWriteStart(local, "xrCreateApiLayerInstance");

        if (!apiLayerInfo || apiLayerInfo->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_CREATE_INFO ||
            apiLayerInfo->structVersion != XR_API_LAYER_CREATE_INFO_STRUCT_VERSION ||
            apiLayerInfo->structSize != sizeof(XrApiLayerCreateInfo) || !apiLayerInfo->nextInfo ||
            apiLayerInfo->nextInfo->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_NEXT_INFO ||
            apiLayerInfo->nextInfo->structVersion != XR_API_LAYER_NEXT_INFO_STRUCT_VERSION ||
            apiLayerInfo->nextInfo->structSize != sizeof(XrApiLayerNextInfo) ||
            apiLayerInfo->nextInfo->layerName != LayerName || !apiLayerInfo->nextInfo->nextGetInstanceProcAddr ||
            !apiLayerInfo->nextInfo->nextCreateApiLayerInstance) {
            Log("xrCreateApiLayerInstance validation failed\n");
            return XR_ERROR_INITIALIZATION_FAILED;
        }

        // Determine if we should entirely bypass the layer for this application.
        {
            std::string baseKey = RegPrefix + "\\" + instanceCreateInfo->applicationInfo.applicationName;

            // Always create a key to make each application name easy to find, and let the user add the bypass key
            // manually.
            {
                char path[_MAX_PATH];
                GetModuleFileNameA(nullptr, path, sizeof(path));
                LAYER_NAMESPACE::utilities::RegSetString(HKEY_CURRENT_USER, xr::utf8_to_wide(baseKey), L"module", path);
            }

            const std::string_view engineName(instanceCreateInfo->applicationInfo.engineName);

            // Bypass the layer if it's either in the no-no list, or if the user requests it.
            const bool bypassLayer =
                engineName == "Chromium" ||
                (LAYER_NAMESPACE::utilities::RegGetDword(HKEY_CURRENT_USER, xr::utf8_to_wide(baseKey), L"bypass")
                     .value_or(0));
            if (bypassLayer) {
                Log("Bypassing OpenXR Toolkit for application '%s', engine '%s'\n",
                    instanceCreateInfo->applicationInfo.applicationName,
                    instanceCreateInfo->applicationInfo.engineName);

                // Bypass interception of xrGetInstanceProcAddr() calls.
                // TODO: What if an application creates multiple instances with different names.
                g_bypass = apiLayerInfo->nextInfo->nextGetInstanceProcAddr;

                // Call the chain to create the instance, and nothing else.
                XrApiLayerCreateInfo chainApiLayerInfo = *apiLayerInfo;
                chainApiLayerInfo.nextInfo = apiLayerInfo->nextInfo->next;
                return apiLayerInfo->nextInfo->nextCreateApiLayerInstance(
                    instanceCreateInfo, &chainApiLayerInfo, instance);
            }
        }

        // Determine whether we are invoked from the OpenXR Developer Tools for Windows Mixed Reality.
        // If we are, we will skip dummy instance create to avoid he XR_LIMIT_REACHED error.
        const bool fastInitialization =
            std::string(instanceCreateInfo->applicationInfo.engineName) == "OpenXRDeveloperTools";

        // Check that the extensions we need are supported by the runtime and/or an upstream API layer.
        //
        // Workaround: per specification, we should be able to retrive the pointer to
        // xrEnumerateInstanceExtensionProperties() without an XrInstance. However, some API layers (eg: Ultraleap) do
        // not seem to properly handle this case. So we create a dummy instance.
        std::set<std::string> extensionsToRequest;
        if (!fastInitialization) {
            XrInstance dummyInstance = XR_NULL_HANDLE;
            PFN_xrEnumerateInstanceExtensionProperties xrEnumerateInstanceExtensionProperties = nullptr;
            PFN_xrGetSystem xrGetSystem = nullptr;
            PFN_xrGetSystemProperties xrGetSystemProperties = nullptr;
            PFN_xrDestroyInstance xrDestroyInstance = nullptr;

            // Try to speed things up by requesting no extentions.
            XrInstanceCreateInfo dummyCreateInfo = *instanceCreateInfo;
            dummyCreateInfo.enabledExtensionCount = dummyCreateInfo.enabledApiLayerCount = 0;

            {
                // Workaround: the Vive API layers are not compliant with xrEnumerateInstanceExtensionProperties()
                // specification and the ability to pass NULL in the first argument.

                // Workaround: the Ultraleap API layer does not seem to properly enumerate the XR_EXT_hand_tracking
                // extension when invoked from within another API layer. We assume the extension is present if we see
                // the API layer.

                auto info = apiLayerInfo->nextInfo;
                while (info && info->next) {
                    std::string_view layerName(info->next->layerName);

                    if (layerName.substr(0, 17) == "XR_APILAYER_VIVE_") {
                        // Skip all the Vive layers.
                        TraceLoggingWriteTagged(
                            local, "xrCreateApiLayerInstance_SkipLayer", TLArg(info->next->layerName, "Layer"));
                        Log("Skipping unsupported layer: %s\n", info->next->layerName);
                        info->nextCreateApiLayerInstance = info->next->nextCreateApiLayerInstance;
                        info->nextGetInstanceProcAddr = info->next->nextGetInstanceProcAddr;
                        info->next = info->next->next;
                    } else {
                        TraceLoggingWriteTagged(
                            local, "xrCreateApiLayerInstance_UseLayer", TLArg(info->next->layerName, "Layer"));
                        Log("Using layer: %s\n", info->next->layerName);

                        if (layerName == "XR_APILAYER_ULTRALEAP_hand_tracking") {
                            // Assume hand tracking extension is present.
                            extensionsToRequest.insert("XR_EXT_hand_tracking");
                        }

                        info = info->next;
                    }
                }
            }

            // Call the chain to create the dummy instance.
            XrApiLayerCreateInfo chainApiLayerInfo = *apiLayerInfo;
            chainApiLayerInfo.nextInfo = apiLayerInfo->nextInfo->next;

            TraceLoggingWriteTagged(local, "xrCreateApiLayerInstance_DummyInstanceCreate");
            const XrResult result = apiLayerInfo->nextInfo->nextCreateApiLayerInstance(
                &dummyCreateInfo, &chainApiLayerInfo, &dummyInstance);
            if (result == XR_SUCCESS) {
                TraceLoggingWriteTagged(local, "xrCreateApiLayerInstance_DummyInstanceCreated");

                CHECK_XRCMD(apiLayerInfo->nextInfo->nextGetInstanceProcAddr(
                    dummyInstance,
                    "xrEnumerateInstanceExtensionProperties",
                    reinterpret_cast<PFN_xrVoidFunction*>(&xrEnumerateInstanceExtensionProperties)));
                CHECK_XRCMD(apiLayerInfo->nextInfo->nextGetInstanceProcAddr(
                    dummyInstance, "xrGetSystem", reinterpret_cast<PFN_xrVoidFunction*>(&xrGetSystem)));
                CHECK_XRCMD(apiLayerInfo->nextInfo->nextGetInstanceProcAddr(
                    dummyInstance,
                    "xrGetSystemProperties",
                    reinterpret_cast<PFN_xrVoidFunction*>(&xrGetSystemProperties)));
                CHECK_XRCMD(apiLayerInfo->nextInfo->nextGetInstanceProcAddr(
                    dummyInstance, "xrDestroyInstance", reinterpret_cast<PFN_xrVoidFunction*>(&xrDestroyInstance)));

                TraceLoggingWriteTagged(
                    local,
                    "xrCreateApiLayerInstance_DummyInstanceProcAddr",
                    TLPArg(xrEnumerateInstanceExtensionProperties, "xrEnumerateInstanceExtensionProperties"),
                    TLPArg(xrGetSystem, "xrGetSystem"),
                    TLPArg(xrGetSystemProperties, "xrGetSystemProperties"),
                    TLPArg(xrDestroyInstance, "xrDestroyInstance"));
            } else {
                TraceLoggingWriteTagged(
                    local, "xrCreateApiLayerInstance_Error_CreateInstance", TLArg((int)result, "Result"));
                Log("Failed to create bootstrap instance: %d\n", result);
            }

            if (xrEnumerateInstanceExtensionProperties) {
                uint32_t extensionsCount = 0;
                CHECK_XRCMD(xrEnumerateInstanceExtensionProperties(nullptr, 0, &extensionsCount, nullptr));
                std::vector<XrExtensionProperties> extensions(extensionsCount, {XR_TYPE_EXTENSION_PROPERTIES});
                CHECK_XRCMD(xrEnumerateInstanceExtensionProperties(
                    nullptr, extensionsCount, &extensionsCount, extensions.data()));
                for (auto extension : extensions) {
                    const std::string extensionName(extension.extensionName);

                    TraceLoggingWriteTagged(
                        local, "xrCreateApiLayerInstance_HasExtension", TLArg(extension.extensionName, "Extension"));
                    Log("Runtime supports extension: %s\n", extension.extensionName);
                    if (extensionName == "XR_EXT_hand_tracking" || extensionName == "XR_EXT_eye_gaze_interaction" ||
                        extensionName == "XR_KHR_win32_convert_performance_counter_time" ||
                        extensionName == "XR_KHR_visibility_mask" || extensionName == "XR_FB_eye_tracking_social") {
                        extensionsToRequest.insert(extensionName);
                    }
                }
            } else {
                Log("Failed to query extensions\n");
            }

            // Workaround: the Vive runtime does not seem to like our flow of destroying the instance
            // mid-initialization. We skip destruction and we will just create a second instance.
            if (xrGetSystem && xrGetSystemProperties) {
                XrSystemGetInfo getInfo{XR_TYPE_SYSTEM_GET_INFO};
                getInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
                XrSystemId systemId;
                if (XR_SUCCEEDED(xrGetSystem(dummyInstance, &getInfo, &systemId))) {
                    XrSystemProperties systemProperties{XR_TYPE_SYSTEM_PROPERTIES};
                    CHECK_XRCMD(xrGetSystemProperties(dummyInstance, systemId, &systemProperties));
                    if (std::string(systemProperties.systemName).find("Vive Reality system") != std::string::npos) {
                        Log("Detected Vive runtime\n");
                        xrDestroyInstance = nullptr;
                    }
                }
            }

            if (xrDestroyInstance) {
                TraceLoggingWriteTagged(local, "xrCreateApiLayerInstance_DummyInstanceDestroy");
                xrDestroyInstance(dummyInstance);
                TraceLoggingWriteTagged(local, "xrCreateApiLayerInstance_DummyInstanceDestroyed");
            }
        }

        // Add the extra extensions to the list of requested extensions when available.
        XrInstanceCreateInfo chainInstanceCreateInfo = *instanceCreateInfo;
        std::vector<const char*> newEnabledExtensionNames;
        if (!fastInitialization) {
            if (!extensionsToRequest.empty()) {
                chainInstanceCreateInfo.enabledExtensionCount += (uint32_t)extensionsToRequest.size();

                newEnabledExtensionNames.resize(chainInstanceCreateInfo.enabledExtensionCount);
                chainInstanceCreateInfo.enabledExtensionNames = newEnabledExtensionNames.data();
                memcpy(newEnabledExtensionNames.data(),
                       instanceCreateInfo->enabledExtensionNames,
                       instanceCreateInfo->enabledExtensionCount * sizeof(const char*));
                uint32_t nextExtensionSlot = instanceCreateInfo->enabledExtensionCount;

                for (auto& extension : extensionsToRequest) {
                    newEnabledExtensionNames[nextExtensionSlot++] = extension.c_str();
                    Log("Requesting extra extension: %s\n", extension.c_str());
                }
            }
        }

        for (uint32_t i = 0; i < chainInstanceCreateInfo.enabledExtensionCount; i++) {
            TraceLoggingWriteTagged(local,
                                    "xrCreateApiLayerInstance_UseExtension",
                                    TLArg(chainInstanceCreateInfo.enabledExtensionNames[i], "Extension"));
        }

        // Call the chain to create the instance.
        XrApiLayerCreateInfo chainApiLayerInfo = *apiLayerInfo;
        chainApiLayerInfo.nextInfo = apiLayerInfo->nextInfo->next;
        TraceLoggingWriteTagged(local, "xrCreateApiLayerInstance_RealInstanceCreate");
        XrResult result =
            apiLayerInfo->nextInfo->nextCreateApiLayerInstance(&chainInstanceCreateInfo, &chainApiLayerInfo, instance);
        if (result == XR_SUCCESS) {
            TraceLoggingWriteTagged(local, "xrCreateApiLayerInstance_RealInstanceCreated");

            // Create our layer.
            LAYER_NAMESPACE::GetInstance()->SetGetInstanceProcAddr(apiLayerInfo->nextInfo->nextGetInstanceProcAddr,
                                                                   *instance);

            // Record the other layers being used here. This is useful when evaluating features based not just on
            // XrInstanceCreateInfo.
            std::vector<std::string> upstreamLayers;
            // We skip the first entry (ourself).
            XrApiLayerNextInfo* entry = apiLayerInfo->nextInfo->next;
            while (entry) {
                upstreamLayers.push_back(entry->layerName);
                entry = entry->next;
            }

            LAYER_NAMESPACE::GetInstance()->SetUpstreamLayers(upstreamLayers);

            result = XR_ERROR_RUNTIME_FAILURE;

            // Forward the xrCreateInstance() call to the layer.
            try {
                result = LAYER_NAMESPACE::GetInstance()->xrCreateInstance(instanceCreateInfo);
            } catch (std::runtime_error& exc) {
                TraceLoggingWriteTagged(local, "xrCreateApiLayerInstance_Error", TLArg(exc.what(), "Error"));
            }

            // Cleanup attempt before returning an error.
            if (XR_FAILED(result)) {
                PFN_xrDestroyInstance xrDestroyInstance = nullptr;
                if (XR_SUCCEEDED(apiLayerInfo->nextInfo->nextGetInstanceProcAddr(
                        *instance, "xrDestroyInstance", reinterpret_cast<PFN_xrVoidFunction*>(&xrDestroyInstance)))) {
                    xrDestroyInstance(*instance);
                }
            }
        }

        TraceLoggingWriteStop(local, "xrCreateApiLayerInstance", TLArg((int)result, "Result"));

        return result;
    }

    // Handle cleanup of the layer's singleton.
    XrResult xrDestroyInstance(XrInstance instance) {
        TraceLocalActivity(local);
        TraceLoggingWriteStart(local, "xrDestroyInstance");

        XrResult result;
        try {
            result = LAYER_NAMESPACE::GetInstance()->xrDestroyInstance(instance);
            if (XR_SUCCEEDED(result)) {
                LAYER_NAMESPACE::ResetInstance();
            }
        } catch (std::runtime_error& exc) {
            TraceLoggingWriteTagged(local, "xrDestroyInstance_Error", TLArg(exc.what(), "Error"));
            result = XR_ERROR_RUNTIME_FAILURE;
        }

        TraceLoggingWriteStop(local, "xrDestroyInstance", TLArg((int)result, "Result"));

        return result;
    }

    // Forward the xrGetInstanceProcAddr() call to the dispatcher.
    XrResult xrGetInstanceProcAddr(XrInstance instance, const char* name, PFN_xrVoidFunction* function) {
        TraceLoggingWrite(g_traceProvider,
                          "xrGetInstanceProcAddr",
                          TLArg(!!g_bypass, "Bypass"),
                          TLPArg(instance, "Instance"),
                          TLArg(name));

        if (g_bypass) {
            return g_bypass(instance, name, function);
        }

        try {
            return LAYER_NAMESPACE::GetInstance()->xrGetInstanceProcAddr(instance, name, function);
        } catch (std::runtime_error& exc) {
            TraceLoggingWrite(g_traceProvider, "xrGetInstanceProcAddr", TLArg(exc.what(), "Error"));
            Log("%s\n", exc.what());
            return XR_ERROR_RUNTIME_FAILURE;
        }
    }

} // namespace LAYER_NAMESPACE
