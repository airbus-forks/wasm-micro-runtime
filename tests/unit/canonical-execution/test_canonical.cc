/*
 * Copyright (C) 2026 Airbus Defence and Space Romania SRL. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include <gtest/gtest.h>
#include <vector>
#include <memory>
#include <cstdio>
#include <cstring>
#include <string>
#include <arpa/inet.h>
#include <errno.h>
#include "../component-instantiation/helpers.h"
extern "C" {
#include "wasm_component_runtime.h"
#include "component-model/wasm_component_host_resource.h"
#include "../../../product-mini/platforms/common/libc_wasi.h"
}

class CanonicalExecutionTest : public testing::Test
{
  public:
    CanonicalExecutionTest() {}
    ~CanonicalExecutionTest() {}
    RuntimeInitArgs init_args;
    unsigned char *component_raw = NULL;
    libc_wasi_parse_context_t parse_ctx;

    char error_buf[128];
    char global_heap_buf[HEAP_SIZE]; // 100 MB

    bool runtime_init = false;

    char input_path[PATH_MAX];
    char output_path[PATH_MAX];

    WASIContext *wasi_ctx;
    char test_dir[128];

    int32_t dir_fd;
    char source_dir[8] = "/source";
    char dest_dir[6] = "/dest";
    char cwd_path[2] = "/";
    char path[PATH_MAX] = "";


    int log_file_fd;

    virtual void SetUp() {
      printf("Starting setup\n");
      memset(&init_args, 0, sizeof(RuntimeInitArgs));

      init_args.mem_alloc_type = Alloc_With_Pool;
      init_args.mem_alloc_option.pool.heap_buf = global_heap_buf;
      init_args.mem_alloc_option.pool.heap_size = sizeof(global_heap_buf);

      libc_wasi_set_default_options(&parse_ctx);

      log_file_fd = dup(STDOUT_FILENO); // save backup of output log file

      if (!wasm_runtime_full_init(&init_args)) {
          printf("Failed to initialize WAMR runtime.\n");
          runtime_init = false;
      } else {
          runtime_init = true;
      }
      bool success = instantiate_host_resource_table();
      ASSERT_TRUE(success);

      char cwd[PATH_MAX];
      getcwd(cwd, sizeof(cwd));
      const char *substr = "wasm-micro-runtime";
      char *pos = strstr(cwd, substr);
      ASSERT_TRUE(pos != NULL) << "Could not find 'wasm-micro-runtime' in cwd";
      size_t prefix_len = (pos - cwd) + strlen(substr);
      strncat(path, cwd, prefix_len);
      strcat(path, "/tests/unit/canonical-execution/test-dir/");
      strcat(input_path, path);
      strcat(output_path, path);
      strcat(input_path, "test_input.txt");
      strcat(output_path, "test_output.txt");

      printf("Ending setup\n");
    }

    virtual void TearDown() {
      printf("Starting teardown\n");
      destroy_host_resource_table();
        if (runtime_init) {
          printf("Starting to destroy runtime\n");
          wasm_runtime_destroy();
          runtime_init = false;
        }
        
        fclose(fopen(output_path, "w"));
        dup2(log_file_fd, STDOUT_FILENO); // return output to original log file
        printf("Ending teardown\n");

    }

    WASMComponent* LoadfromCandidates(const char *file_name) { 
      return load_component_from_candidates_internal(file_name, "canonical-execution");
    }

    void get_test_dir() {
      char cwd[PATH_MAX];
      getcwd(cwd, sizeof(cwd));
      const char *substr = "wasm-micro-runtime";
      char *pos = strstr(cwd, substr);
      if (!pos) {
          printf("Could not find 'wasm-micro-runtime' in cwd\n");
          return;
      }
      size_t prefix_len = (pos - cwd) + strlen(substr);
      test_dir[0] = '\0';
      strncat(test_dir, cwd, prefix_len);
      strcat(test_dir, "/tests/unit/canonical-execution/test-dir");
    }

    void init_prestats(WASMComponentInstance *comp_instance) {

      get_test_dir();

      WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx((WASMModuleInstanceCommon *)comp_instance->core_module_instances[0]);
      wasi_ctx->prestats = (struct fd_prestats *)wasm_runtime_malloc(
          sizeof(struct fd_prestats));
      ASSERT_NE(wasi_ctx->prestats, nullptr);
      memset(wasi_ctx->prestats, 0, sizeof(struct fd_prestats));
      wasi_ctx->prestats->size = 10;
      wasi_ctx->prestats->prestats = (struct fd_prestat *)wasm_runtime_malloc(
          10 * sizeof(struct fd_prestat));
      ASSERT_NE(wasi_ctx->prestats->prestats, nullptr);
      memset(wasi_ctx->prestats->prestats, 0, 10 * sizeof(struct fd_prestat));

      char source_path[PATH_MAX];
      source_path[0] = '\0';
      strcat(source_path, test_dir);
      strcat(source_path, source_dir);
      int32_t source_dir_fd = open(source_path, O_RDONLY | O_DIRECTORY);
      wasi_ctx->prestats->prestats[source_dir_fd].dir = source_dir;

      fd_table_insert_existing(wasi_ctx->curfds, source_dir_fd, source_dir_fd, false);

      char dest_path[PATH_MAX];
      dest_path[0] = '\0';
      strcat(dest_path, test_dir);
      strcat(dest_path, dest_dir);
      int32_t dest_dir_fd = open(dest_path, O_RDONLY | O_DIRECTORY);
      wasi_ctx->prestats->prestats[dest_dir_fd].dir = dest_dir;

      fd_table_insert_existing(wasi_ctx->curfds, dest_dir_fd, dest_dir_fd, false);

      int32_t cwd_fd = open(test_dir, O_RDONLY | O_DIRECTORY);
      wasi_ctx->prestats->prestats[cwd_fd].dir = cwd_path;

      fd_table_insert_existing(wasi_ctx->curfds, cwd_fd, cwd_fd, false);

    }
};

// Test apps made with Rust
TEST_F(CanonicalExecutionTest, test_hello_wat2wasm)
{
  bool status;

  WASMComponent *component = LoadfromCandidates("hello_component.wasm");
  ASSERT_NE(component, nullptr) << "Failed to load/parse component from candidates.";
  
  ASSERT_TRUE(component);
  libc_component_wasi_init(component, 0, NULL, &parse_ctx);

  bh_log_set_verbose_level(WASM_LOG_LEVEL_WARNING);
  // Test component is instantiated
  WASMComponentInstance *comp_instance = wasm_component_instantiate_internal(component, NULL, error_buf, sizeof(error_buf));
  ASSERT_TRUE(comp_instance);

  bh_log_set_verbose_level(WASM_LOG_LEVEL_WARNING);

  char func_name[] = "say-hello()";
  status = wasm_component_application_execute_func(comp_instance, func_name);
  ASSERT_TRUE(status);
}

TEST_F(CanonicalExecutionTest, test_hello_wat2wasm_2)
{
  bool status;

  WASMComponent *component = LoadfromCandidates("hello_component_updated.wasm");
  ASSERT_NE(component, nullptr) << "Failed to load/parse component from candidates.";
  
  ASSERT_TRUE(component);
  libc_component_wasi_init(component, 0, NULL, &parse_ctx);

  // Test component is instantiated
  WASMComponentInstance *comp_instance = wasm_component_instantiate_internal(component, NULL, error_buf, sizeof(error_buf));
  ASSERT_TRUE(comp_instance);

  bh_log_set_verbose_level(WASM_LOG_LEVEL_WARNING);

  char func_name[] = "say-hello()";

  status = wasm_component_application_execute_func(comp_instance, func_name);
  ASSERT_TRUE(status);
}

TEST_F(CanonicalExecutionTest, test_hello_rust)
{
  bool status;
  
  WASMComponent *component = LoadfromCandidates("hello_world.wasm");
  ASSERT_NE(component, nullptr) << "Failed to load/parse component from candidates.";
  
  ASSERT_TRUE(component);
  libc_component_wasi_init(component, 0, NULL, &parse_ctx);

  // Test component is instantiated
  WASMComponentInstance *comp_instance = wasm_component_instantiate_internal(component, NULL, error_buf, sizeof(error_buf));
  ASSERT_TRUE(comp_instance);

  bh_log_set_verbose_level(WASM_LOG_LEVEL_WARNING);

  char func_name[] = "run()";

  status = wasm_component_application_execute_func(comp_instance, func_name);
  ASSERT_TRUE(status);
}

TEST_F(CanonicalExecutionTest, test_surface)
{
  bool status;

  WASMComponent *component = LoadfromCandidates("surface_and_geometry_0_4_0.wasm");
  ASSERT_NE(component, nullptr) << "Failed to load/parse component from candidates.";
  
  ASSERT_TRUE(component);
  libc_component_wasi_init(component, 0, NULL, &parse_ctx);

  bh_log_set_verbose_level(WASM_LOG_LEVEL_WARNING);
  // Test component is instantiated
  WASMComponentInstance *comp_instance = wasm_component_instantiate_internal(component, NULL, error_buf, sizeof(error_buf));
  ASSERT_TRUE(comp_instance);

  bh_log_set_verbose_level(WASM_LOG_LEVEL_WARNING);

  char func_name[] = "test-describe-shape()";
  status = wasm_component_application_execute_func(comp_instance, func_name);
  ASSERT_TRUE(status);
}

TEST_F(CanonicalExecutionTest, test_wasi_random)
{
  bool status;

  WASMComponent *component = LoadfromCandidates("wasi_random.wasm");
  ASSERT_NE(component, nullptr) << "Failed to load/parse component from candidates.";
  
  ASSERT_TRUE(component);
  libc_component_wasi_init(component, 0, NULL, &parse_ctx);

  bh_log_set_verbose_level(WASM_LOG_LEVEL_WARNING);
  // Test component is instantiated
  WASMComponentInstance *comp_instance = wasm_component_instantiate_internal(component, NULL, error_buf, sizeof(error_buf));
  ASSERT_TRUE(comp_instance);

  bh_log_set_verbose_level(WASM_LOG_LEVEL_WARNING);

  char func_name[] = "run()";
  status = wasm_component_application_execute_func(comp_instance, func_name);
  ASSERT_TRUE(status);
}

TEST_F(CanonicalExecutionTest, test_wasi_clocks)
{
  bool status;

  WASMComponent *component = LoadfromCandidates("wasi_clocks.wasm");
  ASSERT_NE(component, nullptr) << "Failed to load/parse component from candidates.";
  
  ASSERT_TRUE(component);
  libc_component_wasi_init(component, 0, NULL, &parse_ctx);

  bh_log_set_verbose_level(WASM_LOG_LEVEL_WARNING);
  // Test component is instantiated
  WASMComponentInstance *comp_instance = wasm_component_instantiate_internal(component, NULL, error_buf, sizeof(error_buf));
  ASSERT_TRUE(comp_instance);

  bh_log_set_verbose_level(WASM_LOG_LEVEL_WARNING);

  char func_name[] = "run()";
  status = wasm_component_application_execute_func(comp_instance, func_name);
  ASSERT_TRUE(status);
}

TEST_F(CanonicalExecutionTest, test_wasi_cli)
{
  bool status;

  WASMComponent *component = LoadfromCandidates("wasi_cli.wasm");
  ASSERT_NE(component, nullptr) << "Failed to load/parse component from candidates.";
  
  ASSERT_TRUE(component);
  libc_component_wasi_init(component, 0, NULL, &parse_ctx);

  bh_log_set_verbose_level(WASM_LOG_LEVEL_WARNING);
  // Test component is instantiated
  WASMComponentInstance *comp_instance = wasm_component_instantiate_internal(component, NULL, error_buf, sizeof(error_buf));
  ASSERT_TRUE(comp_instance);

  bh_log_set_verbose_level(WASM_LOG_LEVEL_WARNING);

  // Redirect stdin and stdout
  FILE *input_file = freopen(input_path, "r", stdin);
  ASSERT_TRUE(input_file);

  FILE *output_file = freopen(output_path, "w", stdout);
  ASSERT_TRUE(output_file);

  // Call function with no arguments
  char func_name[] = "run()";
  status = wasm_component_application_execute_func(comp_instance, func_name);
  ASSERT_TRUE(status);

  FILE *output = fopen(output_path, "r");
  ASSERT_TRUE(output);

  char line[256];
  fgets(line, sizeof(line), output);
  ASSERT_TRUE(strstr(line, "Arguments (0):"));
  fgets(line, sizeof(line), output);
  ASSERT_TRUE(strstr(line, "Environment variables (0):"));
}

TEST_F(CanonicalExecutionTest, test_wasi_cli_with_args)
{
  bool status;

  WASMComponent *component = LoadfromCandidates("wasi_cli.wasm");
  ASSERT_NE(component, nullptr) << "Failed to load/parse component from candidates.";
  
  ASSERT_TRUE(component);
  char *argv = (char *)"arg1";

  parse_ctx.env_list[0] = "MY_VAR=hello";
  parse_ctx.env_list[1] = "ANOTHER=world";
  parse_ctx.env_list_size = 2;

  ASSERT_FALSE(libc_wasi_parse_options("inherit-env=n", &parse_ctx));

  libc_component_wasi_init(component, 1, &argv, &parse_ctx);

  bh_log_set_verbose_level(WASM_LOG_LEVEL_WARNING);
  // Test component is instantiated
  WASMComponentInstance *comp_instance = wasm_component_instantiate_internal(component, NULL, error_buf, sizeof(error_buf));
  ASSERT_TRUE(comp_instance);

  bh_log_set_verbose_level(WASM_LOG_LEVEL_WARNING);

  // Redirect stdin and stdout
  FILE *input_file = freopen(input_path, "r", stdin);
  ASSERT_TRUE(input_file);

  FILE *output_file = freopen(output_path, "w", stdout);
  ASSERT_TRUE(output_file);

  // Call function with no arguments
  char func_name[] = "run()";
  status = wasm_component_application_execute_func(comp_instance, func_name);
  ASSERT_TRUE(status);

  FILE *output = fopen(output_path, "r");
  ASSERT_TRUE(output);

  char line[256][256];
  uint32_t i = 0;
  while(fgets(line[i], sizeof(line[i]), output)) i++;

  ASSERT_TRUE(strstr(line[0], "Arguments (1):"));
  ASSERT_TRUE(strstr(line[1], "arg1"));
  ASSERT_TRUE(strstr(line[2], "Environment variables (2):"));
  ASSERT_EQ(i, 13);
}

TEST_F(CanonicalExecutionTest, test_wasi_cli_inherit_env)
{
  bool status;

  WASMComponent *component = LoadfromCandidates("wasi_cli.wasm");
  ASSERT_NE(component, nullptr) << "Failed to load/parse component from candidates.";
  
  ASSERT_TRUE(component);
  char *argv = (char *)"arg1";

  parse_ctx.env_list[0] = "MY_VAR=hello";
  parse_ctx.env_list[1] = "ANOTHER=world";
  parse_ctx.env_list_size = 2;

  ASSERT_FALSE(libc_wasi_parse_options("inherit-env=y", &parse_ctx));

  libc_component_wasi_init(component, 1, &argv, &parse_ctx);

  bh_log_set_verbose_level(WASM_LOG_LEVEL_WARNING);
  // Test component is instantiated
  WASMComponentInstance *comp_instance = wasm_component_instantiate_internal(component, NULL, error_buf, sizeof(error_buf));
  ASSERT_TRUE(comp_instance);

  bh_log_set_verbose_level(WASM_LOG_LEVEL_WARNING);

  // Redirect stdin and stdout
  FILE *input_file = freopen(input_path, "r", stdin);
  ASSERT_TRUE(input_file);

  FILE *output_file = freopen(output_path, "w", stdout);
  ASSERT_TRUE(output_file);

  // Call function with no arguments
  char func_name[] = "run()";
  status = wasm_component_application_execute_func(comp_instance, func_name);
  ASSERT_TRUE(status);

  FILE *output = fopen(output_path, "r");
  ASSERT_TRUE(output);

  char line[256][256];
  uint32_t i = 0;
  while(fgets(line[i], sizeof(line[i]), output)) i++;

  ASSERT_TRUE(strstr(line[0], "Arguments (1):"));
  ASSERT_TRUE(strstr(line[1], "arg1"));
  ASSERT_TRUE(i > 13);
}

TEST_F(CanonicalExecutionTest, test_wasi_filesystem)
{
  bool status;

  WASMComponent *component = LoadfromCandidates("wasi_filesystem.wasm");
  ASSERT_NE(component, nullptr) << "Failed to load/parse component from candidates.";
  
  ASSERT_TRUE(component);

  ASSERT_FALSE(libc_wasi_parse_options("inherit-env=y", &parse_ctx));

  libc_component_wasi_init(component, 0, NULL, &parse_ctx);

  bh_log_set_verbose_level(WASM_LOG_LEVEL_WARNING);
  // Test component is instantiated
  WASMComponentInstance *comp_instance = wasm_component_instantiate_internal(component, NULL, error_buf, sizeof(error_buf));
  ASSERT_TRUE(comp_instance);

  init_prestats(comp_instance);

  bh_log_set_verbose_level(WASM_LOG_LEVEL_WARNING);

  char func_name[] = "run()";
  status = wasm_component_application_execute_func(comp_instance, func_name);
  ASSERT_TRUE(status);
}

// Test apps made with C WASi-SDK
TEST_F(CanonicalExecutionTest, test_wasi_random_c)
{
  bool status;

  WASMComponent *component = LoadfromCandidates("wasi_random_c.wasm");
  ASSERT_NE(component, nullptr) << "Failed to load/parse component from candidates.";
  
  ASSERT_TRUE(component);
  libc_component_wasi_init(component, 0, NULL, &parse_ctx);

  bh_log_set_verbose_level(WASM_LOG_LEVEL_WARNING);
  // Test component is instantiated
  WASMComponentInstance *comp_instance = wasm_component_instantiate_internal(component, NULL, error_buf, sizeof(error_buf));
  ASSERT_TRUE(comp_instance);

  bh_log_set_verbose_level(WASM_LOG_LEVEL_WARNING);

  char func_name[] = "run()";
  status = wasm_component_application_execute_func(comp_instance, func_name);
  ASSERT_TRUE(status);
}

TEST_F(CanonicalExecutionTest, test_wasi_clocks_c)
{
  bool status;

  WASMComponent *component = LoadfromCandidates("wasi_clocks_c.wasm");
  ASSERT_NE(component, nullptr) << "Failed to load/parse component from candidates.";
  
  ASSERT_TRUE(component);
  libc_component_wasi_init(component, 0, NULL, &parse_ctx);

  bh_log_set_verbose_level(WASM_LOG_LEVEL_WARNING);
  // Test component is instantiated
  WASMComponentInstance *comp_instance = wasm_component_instantiate_internal(component, NULL, error_buf, sizeof(error_buf));
  ASSERT_TRUE(comp_instance);

  bh_log_set_verbose_level(WASM_LOG_LEVEL_WARNING);

  char func_name[] = "run()";
  status = wasm_component_application_execute_func(comp_instance, func_name);
  ASSERT_TRUE(status);
}

TEST_F(CanonicalExecutionTest, test_wasi_cli_c)
{
  bool status;

  WASMComponent *component = LoadfromCandidates("wasi_cli_c.wasm");
  ASSERT_NE(component, nullptr) << "Failed to load/parse component from candidates.";
  
  ASSERT_TRUE(component);
  char *argv = (char *)"arg1";

  parse_ctx.env_list[0] = "MY_VAR=hello";
  parse_ctx.env_list[1] = "ANOTHER=world";
  parse_ctx.env_list_size = 2;

  ASSERT_FALSE(libc_wasi_parse_options("inherit-env=n", &parse_ctx));

  libc_component_wasi_init(component, 1, &argv, &parse_ctx);

  bh_log_set_verbose_level(WASM_LOG_LEVEL_WARNING);
  // Test component is instantiated
  WASMComponentInstance *comp_instance = wasm_component_instantiate_internal(component, NULL, error_buf, sizeof(error_buf));
  ASSERT_TRUE(comp_instance);

  bh_log_set_verbose_level(WASM_LOG_LEVEL_WARNING);

  // Redirect stdin and stdout
  FILE *input_file = freopen(input_path, "r", stdin);
  ASSERT_TRUE(input_file);

  FILE *output_file = freopen(output_path, "w", stdout);
  ASSERT_TRUE(output_file);

  // Call function with no arguments
  char func_name[] = "run()";
  status = wasm_component_application_execute_func(comp_instance, func_name);
  ASSERT_TRUE(status);

  FILE *output = fopen(output_path, "r");
  ASSERT_TRUE(output);

  char line[256][256];
  uint32_t i = 0;
  while(fgets(line[i], sizeof(line[i]), output)) i++;

  ASSERT_TRUE(strstr(line[1], "CWD is :"));
  ASSERT_TRUE(strstr(line[2], "Get environmnet variables: "));
  ASSERT_TRUE(strstr(line[3], "MY_VAR=hello"));
  ASSERT_TRUE(strstr(line[6], "You entered: test"));
  ASSERT_TRUE(strstr(line[7], "Found 1 argument"));
  ASSERT_TRUE(strstr(line[8], "arg1"));
}

TEST_F(CanonicalExecutionTest, test_wasi_filesystem_c)
{
  bool status;

  WASMComponent *component = LoadfromCandidates("wasi_filesystem_c.wasm");
  ASSERT_NE(component, nullptr) << "Failed to load/parse component from candidates.";
  
  ASSERT_TRUE(component);

  ASSERT_FALSE(libc_wasi_parse_options("inherit-env=y", &parse_ctx));

  libc_component_wasi_init(component, 0, NULL, &parse_ctx);

  bh_log_set_verbose_level(WASM_LOG_LEVEL_WARNING);
  // Test component is instantiated
  WASMComponentInstance *comp_instance = wasm_component_instantiate_internal(component, NULL, error_buf, sizeof(error_buf));
  ASSERT_TRUE(comp_instance);

  init_prestats(comp_instance);

  bh_log_set_verbose_level(WASM_LOG_LEVEL_WARNING);

  char func_name[] = "run()";
  status = wasm_component_application_execute_func(comp_instance, func_name);
  ASSERT_TRUE(status);
}

TEST_F(CanonicalExecutionTest, test_wasi_filesystem_sandboxing)
{
  bool status;
  const char* file_name = "test_filesystem_paths_comp.wasm";
  WASMComponent *component = LoadfromCandidates(file_name);
  ASSERT_NE(component, nullptr) << "Failed to load/parse component from candidates.";
  ASSERT_TRUE(component);

  ASSERT_FALSE(libc_wasi_parse_options("inherit-env=y", &parse_ctx));

  char absolute_escaped_path[PATH_MAX];
  strcat(absolute_escaped_path, path);
  strcat(absolute_escaped_path, "example.txt");

  const char *wasm_argv[] = {
      file_name, 
      "--",                         
      absolute_escaped_path                     
  };
  int wasm_argc = 3;

  libc_component_wasi_init(component, wasm_argc, (char **)wasm_argv, &parse_ctx);

  bh_log_set_verbose_level(WASM_LOG_LEVEL_WARNING);

  WASMComponentInstance *comp_instance = wasm_component_instantiate_internal(component, NULL, error_buf, sizeof(error_buf));
  ASSERT_TRUE(comp_instance);

  init_prestats(comp_instance);

  bh_log_set_verbose_level(WASM_LOG_LEVEL_WARNING);

  char func_name[] = "run()";
  status = wasm_component_application_execute_func(comp_instance, func_name);
  
  ASSERT_TRUE(status);
}