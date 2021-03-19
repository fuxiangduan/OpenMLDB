/*
 * Copyright 2021 4Paradigm
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

package com._4paradigm.hybridse.common;

import com._4paradigm.hybridse.HybridSELibrary;
import com._4paradigm.hybridse.vm.Engine;
import com._4paradigm.hybridse.vm.HybridSEJITWrapper;
import com._4paradigm.hybridse.vm.JITOptions;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import java.io.IOException;
import java.io.InputStream;
import java.nio.ByteBuffer;
import java.util.*;


public class JITManager {

    static private Logger logger = LoggerFactory.getLogger(JITManager.class);

    static {
        HybridSELibrary.initCore();
        Engine.InitializeGlobalLLVM();
    }

    // One jit currently only take one llvm module, since symbol may duplicate
    static private Map<String, HybridSEJITWrapper> jits = new HashMap<>();
    static private Set<String> initializedModuleTags = new HashSet<>();

    synchronized static public HybridSEJITWrapper getJIT(String tag) {
        if (! jits.containsKey(tag)) {
            HybridSEJITWrapper jit = HybridSEJITWrapper.Create(getJITOptions());
            if (jit == null) {
                throw new RuntimeException("Fail to create native jit");
            }
            jit.Init();
            HybridSEJITWrapper.InitJITSymbols(jit);
            jits.put(tag, jit);
        }
        return jits.get(tag);
    }

    static private JITOptions getJITOptions() {
        JITOptions options = new JITOptions();
        try (InputStream input = JITManager.class.getClassLoader().getResourceAsStream(
                "jit.properties")) {
            Properties prop = new Properties(System.getProperties());
            if (input == null) {
                return options;
            }
            prop.load(input);
            String enableMCJIT = prop.getProperty("fesql.jit.enable_mcjit");
            if (enableMCJIT != null && enableMCJIT.toLowerCase().equals("true")) {
                logger.info("Try enable llvm legacy mcjit support");
                options.set_enable_mcjit(true);
            }
            String enableVTune = prop.getProperty("fesql.jit.enable_vtune");
            if (enableVTune != null && enableVTune.toLowerCase().equals("true")) {
                logger.info("Try enable intel jit events support");
                options.set_enable_vtune(true);
            }
            String enablePerf = prop.getProperty("fesql.jit.enable_perf");
            if (enablePerf != null && enablePerf.toLowerCase().equals("true")) {
                logger.info("Try enable perf jit events support");
                options.set_enable_perf(true);
            }
            String enableGdb = prop.getProperty("fesql.jit.enable_gdb");
            if (enableGdb != null && enableGdb.toLowerCase().equals("true")) {
                logger.info("Try enable gdb jit events support");
                options.set_enable_gdb(true);
            }
        } catch (IOException ex) {
            logger.debug("Can not find jit.properties", ex);
        }
        return options;
    }

    synchronized static private boolean hasModule(String tag) {
        return initializedModuleTags.contains(tag);
    }

    synchronized static private void initModule(String tag, ByteBuffer moduleBuffer) {
        HybridSEJITWrapper jit = getJIT(tag);
        if (! moduleBuffer.isDirect()) {
            throw new RuntimeException("JIT must use direct buffer");
        }
        if (!jit.AddModuleFromBuffer(moduleBuffer)) {
            throw new RuntimeException("Fail to initialize native module");
        }
        initializedModuleTags.add(tag);
    }

    synchronized static public void initJITModule(String tag, ByteBuffer moduleBuffer) {

        // ensure worker native
        HybridSELibrary.initCore();

        // ensure worker side module
        if (!JITManager.hasModule(tag)) {
            JITManager.initModule(tag, moduleBuffer);
            logger.info("Init jit module with tag:\n" + tag);
        }
    }

    synchronized static public void removeModule(String tag) {
        initializedModuleTags.remove(tag);
        HybridSEJITWrapper jit = jits.remove(tag);
        if (jit != null) {
            HybridSEJITWrapper.DeleteJIT(jit);
            jit.delete();
        }
    }

    synchronized static public void clear() {
        initializedModuleTags.clear();
        for (Map.Entry<String, HybridSEJITWrapper> entry : jits.entrySet()) {
            HybridSEJITWrapper.DeleteJIT(entry.getValue());
            entry.getValue().delete();
        }
        jits.clear();
    }
}
