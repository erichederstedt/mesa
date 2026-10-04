#include "zgfx_private.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ac_binary.h"
#include "ac_shader_args.h"
#include "ac_shader_util.h"
#include "compiler/aco_interface.h"
#include "compiler/glsl_types.h"
#include "compiler/nir/nir_builder.h"
#include "compiler/spirv/nir_spirv.h"
#include "compiler/spirv/spirv_info.h"
#include "nir/ac_nir.h"
#include "sid.h"
#include "util/macros.h"

struct zgfx_shader {
   zgfx_device *dev;
   void *code;
   uint32_t code_size;
   uint32_t exec_size;
   struct ac_shader_config config;
   uint32_t workgroup_size[3];
   uint32_t wave_size;
   uint32_t num_user_sgprs;
   struct ac_shader_args args;
   struct ac_arg argument_ptr;
};

static bool shader_check_spirv(const uint32_t *words, size_t count)
{
   if (words[0] != SpvMagicNumber)
      return false;

   for (size_t i = 5; i < count;) {
      unsigned length = words[i] >> 16;
      unsigned opcode = words[i] & 0xffff;
      if (!length || length > count - i)
         return false;
      if (opcode == SpvOpCapability) {
         if (length != 2)
            return false;
         switch (words[i + 1]) {
         case SpvCapabilityShader:
         case SpvCapabilityMatrix:
         case SpvCapabilityInt64:
         case SpvCapabilityPhysicalStorageBufferAddresses:
            break;
         default:
            fprintf(stderr, "zgfx: unsupported SPIR-V capability %u\n", words[i + 1]);
            return false;
         }
      }
      i += length;
   }
   return true;
}

static void shader_spirv_message(void *data, enum nir_spirv_debug_level level,
                                 size_t offset, const char *message)
{
   if (level >= NIR_SPIRV_DEBUG_LEVEL_WARNING)
      fprintf(stderr, "zgfx: SPIR-V at byte %zu: %s\n", offset, message);
}

static void shader_optimize(nir_shader *nir)
{
   bool progress;
   do {
      progress = false;
      NIR_PASS(progress, nir, nir_lower_vars_to_ssa);
      NIR_PASS(progress, nir, nir_opt_copy_prop);
      NIR_PASS(progress, nir, nir_opt_remove_phis);
      NIR_PASS(progress, nir, nir_opt_dce);
      NIR_PASS(progress, nir, nir_opt_dead_cf);
      NIR_PASS(progress, nir, nir_opt_cse);
      NIR_PASS(progress, nir, nir_opt_algebraic);
      NIR_PASS(progress, nir, nir_opt_constant_folding);
      NIR_PASS(progress, nir, nir_opt_if, nir_opt_if_optimize_phi_true_false);
   } while (progress);
}

static bool shader_lower_arguments(nir_builder *b, nir_intrinsic_instr *intrin, void *data)
{
   zgfx_shader *shader = data;
   if (intrin->intrinsic != nir_intrinsic_load_push_constant)
      return false;

   b->cursor = nir_before_instr(&intrin->instr);
   nir_def *ptr = ac_nir_load_arg(b, &shader->args, shader->argument_ptr);
   unsigned offset = nir_intrinsic_base(intrin) + nir_src_as_uint(intrin->src[0]);
   nir_def *value = nir_extract_bits(b, &ptr, 1, offset * 8,
                                     intrin->def.num_components, intrin->def.bit_size);
   nir_def_replace(&intrin->def, value);
   return true;
}

static void shader_build_binary(void **data, const aco_callback_params *params)
{
   zgfx_shader *shader = *data;
   if (params->config.scratch_bytes_per_wave) {
      fprintf(stderr, "zgfx: shaders requiring scratch memory are not supported yet\n");
      return;
   }
   for (unsigned i = 0; i < params->num_symbols; i++) {
      if (params->symbols[i].id != aco_symbol_const_data_addr) {
         fprintf(stderr, "zgfx: unsupported shader relocation\n");
         return;
      }
   }
   if (!params->code_dw || params->code_dw > UINT32_MAX / 4)
      return;
   shader->code_size = params->code_dw * 4;
   shader->code = malloc(shader->code_size);
   if (!shader->code)
      return;
   memcpy(shader->code, params->code, shader->code_size);
   shader->config = params->config;
   shader->exec_size = params->exec_size;
   shader->wave_size = params->wave_size;
}

PUBLIC zgfx_shader *shader_create(zgfx_device *dev, uint8_t *spriv_bytes,
                                  uint64_t spriv_size, zgfx_shader_type shader_type, char *entry)
{
   if (!dev || !spriv_bytes || spriv_size < 20 || spriv_size > SIZE_MAX ||
       spriv_size % 4 || !entry || !*entry || shader_type != ZGFX_SHADER_COMPUTE) {
      fprintf(stderr, "zgfx: shader_create requires compute SPIR-V and an entry point\n");
      return NULL;
   }
   if (!aco_is_gpu_supported(&dev->info)) {
      fprintf(stderr, "zgfx: GPU is not supported by ACO\n");
      return NULL;
   }

   uint32_t *words = malloc(spriv_size);
   if (!words)
      return NULL;
   memcpy(words, spriv_bytes, spriv_size);
   if (!shader_check_spirv(words, spriv_size / 4)) {
      fprintf(stderr, "zgfx: invalid or unsupported SPIR-V\n");
      free(words);
      return NULL;
   }

   nir_shader_compiler_options nir_options = {0};
   ac_nir_set_options(&dev->info.compiler_info, false, &nir_options);
   nir_options.max_unroll_iterations = 32;
   const struct spirv_capabilities capabilities = {
      .Shader = true,
      .Matrix = true,
      .Int64 = true,
      .PhysicalStorageBufferAddresses = true,
   };
   const struct spirv_to_nir_options spirv_options = {
      .environment = NIR_SPIRV_VULKAN,
      .capabilities = &capabilities,
      .ubo_addr_format = nir_address_format_vec2_index_32bit_offset,
      .ssbo_addr_format = nir_address_format_vec2_index_32bit_offset,
      .phys_ssbo_addr_format = nir_address_format_64bit_global,
      .push_const_addr_format = nir_address_format_logical,
      .shared_addr_format = nir_address_format_32bit_offset,
      .global_addr_format = nir_address_format_64bit_global,
      .constant_addr_format = nir_address_format_64bit_global,
      .skip_os_break_in_debug_build = true,
      .debug.func = shader_spirv_message,
   };
   glsl_type_singleton_init_or_ref();
   nir_shader *nir = spirv_to_nir(words, spriv_size / 4, NULL, MESA_SHADER_COMPUTE,
                                  entry, &spirv_options, &nir_options);
   free(words);
   zgfx_shader *shader = NULL;
   if (!nir)
      goto fail;

   nir_foreach_variable_in_shader(var, nir) {
      if (var->data.mode & (nir_var_uniform | nir_var_image | nir_var_mem_ubo | nir_var_mem_ssbo)) {
         fprintf(stderr, "zgfx: shader descriptors are not supported; use physical pointers\n");
         goto fail;
      }
      if (var->data.mode == nir_var_mem_push_const && glsl_get_explicit_size(var->type, false) > 8) {
         fprintf(stderr, "zgfx: push constants must fit the 8-byte argument pointer\n");
         goto fail;
      }
   }
   uint64_t workgroup_size = 1;
   for (unsigned i = 0; i < 3; i++) {
      if (!nir->info.workgroup_size[i] || nir->info.workgroup_size[i] > 1024)
         goto fail;
      workgroup_size *= nir->info.workgroup_size[i];
   }
   if (nir->info.workgroup_size_variable || workgroup_size > 1024) {
      fprintf(stderr, "zgfx: unsupported compute workgroup size\n");
      goto fail;
   }

   shader = calloc(1, sizeof(*shader));
   if (!shader)
      goto fail;
   shader->dev = dev;
   shader->wave_size = 64;
   nir->info.api_subgroup_size = shader->wave_size;
   nir->info.min_subgroup_size = shader->wave_size;
   nir->info.max_subgroup_size = shader->wave_size;
   for (unsigned i = 0; i < 3; i++)
      shader->workgroup_size[i] = nir->info.workgroup_size[i];

   NIR_PASS(_, nir, nir_lower_variable_initializers, nir_var_function_temp);
   NIR_PASS(_, nir, nir_lower_returns);
   NIR_PASS(_, nir, nir_inline_functions);
   nir_remove_non_entrypoints(nir);
   NIR_PASS(_, nir, nir_lower_disordered_control_barriers);
   NIR_PASS(_, nir, nir_opt_deref);
   NIR_PASS(_, nir, nir_lower_variable_initializers, ~0);
   NIR_PASS(_, nir, nir_split_var_copies);
   NIR_PASS(_, nir, nir_split_per_member_structs);
   NIR_PASS(_, nir, nir_lower_global_vars_to_local);
   NIR_PASS(_, nir, nir_lower_var_copies);
   NIR_PASS(_, nir, nir_lower_memcpy);
   NIR_PASS(_, nir, nir_lower_vars_to_ssa);
   NIR_PASS(_, nir, nir_lower_system_values);
   NIR_PASS(_, nir, nir_lower_compute_system_values,
            &(nir_lower_compute_system_values_options){.lower_local_invocation_index = true});
   NIR_PASS(_, nir, nir_lower_explicit_io, nir_var_mem_push_const, nir_address_format_32bit_offset);
   NIR_PASS(_, nir, nir_lower_vars_to_explicit_types, nir_var_mem_shared, glsl_get_natural_size_align_bytes);
   NIR_PASS(_, nir, nir_lower_explicit_io, nir_var_mem_shared, nir_address_format_32bit_offset);
   NIR_PASS(_, nir, nir_lower_explicit_io, nir_var_mem_global | nir_var_mem_constant,
            nir_address_format_64bit_global);
   NIR_PASS(_, nir, ac_nir_lower_indirect_derefs);
   if (nir->scratch_size) {
      fprintf(stderr, "zgfx: shaders requiring scratch memory are not supported yet\n");
      goto fail;
   }
   NIR_PASS(_, nir, nir_normalize_sin_cos);
   shader_optimize(nir);
   NIR_PASS(_, nir, nir_lower_flrp, 16 | 32 | 64, false);
   nir_shader_gather_info(nir, nir_shader_get_entrypoint(nir));
   if (nir->info.shared_size > dev->info.compiler_info.lds_size_per_workgroup) {
      fprintf(stderr, "zgfx: shader exceeds the GPU shared-memory limit\n");
      goto fail;
   }

   struct ac_shader_args *args = &shader->args;
   ac_add_arg(args, AC_ARG_SGPR, 2, AC_ARG_VALUE, &shader->argument_ptr);
   if (BITSET_TEST(nir->info.system_values_read, SYSTEM_VALUE_NUM_WORKGROUPS))
      ac_add_arg(args, AC_ARG_SGPR, 3, AC_ARG_VALUE, &args->num_work_groups);
   shader->num_user_sgprs = args->num_sgprs_used;
   for (unsigned i = 0; i < 3; i++) {
      if (dev->info.gfx_level >= GFX12)
         args->workgroup_ids[i].used = true;
      else
         ac_add_arg(args, AC_ARG_SGPR, 1, AC_ARG_VALUE, &args->workgroup_ids[i]);
   }
   if (dev->info.compiler_info.local_invocation_ids_packed) {
      ac_add_arg(args, AC_ARG_VGPR, 1, AC_ARG_VALUE, &args->local_invocation_ids_packed);
   } else {
      ac_add_arg(args, AC_ARG_VGPR, 1, AC_ARG_VALUE, &args->local_invocation_id_x);
      ac_add_arg(args, AC_ARG_VGPR, 1, AC_ARG_VALUE, &args->local_invocation_id_y);
      ac_add_arg(args, AC_ARG_VGPR, 1, AC_ARG_VALUE, &args->local_invocation_id_z);
   }

   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
            if (intrin->intrinsic != nir_intrinsic_load_push_constant)
               continue;
            uint64_t size = intrin->def.num_components * intrin->def.bit_size / 8;
            if (!nir_src_is_const(intrin->src[0]) ||
                (uint64_t)nir_intrinsic_base(intrin) + nir_src_as_uint(intrin->src[0]) + size > 8) {
               fprintf(stderr, "zgfx: push-constant access exceeds the argument pointer\n");
               goto fail;
            }
         }
      }
   }
   nir_shader_intrinsics_pass(nir, shader_lower_arguments, nir_metadata_control_flow, shader);
   NIR_PASS(_, nir, nir_lower_memory_model);
   NIR_PASS(_, nir, nir_lower_idiv, &(nir_lower_idiv_options){.allow_fp16 = false});
   NIR_PASS(_, nir, ac_nir_lower_intrinsics_to_args, args,
            &(ac_nir_lower_intrinsics_to_args_options){
               .gfx_level = dev->info.gfx_level,
               .hw_stage = AC_HW_COMPUTE_SHADER,
               .wave_size = shader->wave_size,
               .workgroup_size = workgroup_size,
               .load_grid_size_from_user_sgpr = true,
            });
   NIR_PASS(_, nir, ac_nir_lower_mem_access_bit_sizes, dev->info.gfx_level, false);
   NIR_PASS(_, nir, ac_nir_lower_global_access, dev->info.gfx_level);
   NIR_PASS(_, nir, nir_lower_int64);
   shader_optimize(nir);
   while (nir_opt_algebraic_late(nir)) {
      NIR_PASS(_, nir, nir_opt_constant_folding);
      NIR_PASS(_, nir, nir_opt_copy_prop);
      NIR_PASS(_, nir, nir_opt_dce);
      NIR_PASS(_, nir, nir_opt_cse);
   }
   if (ac_nir_might_lower_bit_size(nir)) {
      if (dev->info.gfx_level >= GFX8)
         nir_divergence_analysis(nir);
      NIR_PASS(_, nir, nir_lower_bit_size, ac_nir_lower_bit_size_callback, &dev->info.gfx_level);
   }
   NIR_PASS(_, nir, nir_lower_alu_width, ac_nir_opt_vectorize_cb, &dev->info.gfx_level);
   NIR_PASS(_, nir, nir_lower_load_const_to_scalar);
   NIR_PASS(_, nir, nir_opt_copy_prop);
   NIR_PASS(_, nir, nir_opt_dce);
   nir_validate_shader(nir, "zgfx before ACO");

   const struct aco_compiler_options aco_options = {
      .compiler_info = &dev->info.compiler_info,
      .family = dev->info.family,
      .gfx_level = dev->info.gfx_level,
      .address32_hi = dev->info.address32_hi,
   };
   const struct aco_shader_info aco_info = {
      .hw_stage = AC_HW_COMPUTE_SHADER,
      .wave_size = shader->wave_size,
      .workgroup_size = workgroup_size,
      .lds_size = nir->info.shared_size,
   };
   void *binary = shader;
   aco_compile_shader(&aco_options, &aco_info, 1, &nir, args, shader_build_binary, &binary);
   if (!shader->code)
      goto fail;

   struct ac_shader_config *config = &shader->config;
   unsigned vgpr_granularity = dev->info.compiler_info.wave64_vgpr_encode_granularity;
   unsigned vgprs = MAX2(config->num_vgprs, args->num_vgprs_used);
   unsigned sgprs = MAX2(config->num_sgprs, args->num_sgprs_used);
   config->rsrc1 = S_00B848_VGPRS(DIV_ROUND_UP(vgprs, vgpr_granularity) - 1) |
                  S_00B848_FLOAT_MODE(config->float_mode) |
                  S_00B848_DX10_CLAMP(dev->info.gfx_level < GFX11_7);
   if (dev->info.gfx_level < GFX10)
      config->rsrc1 |= S_00B848_SGPRS(DIV_ROUND_UP(sgprs, 8) - 1);
   if (dev->info.gfx_level >= GFX10 && dev->info.gfx_level <= GFX11_7)
      config->rsrc1 |= S_00B848_MEM_ORDERED(config->mem_ordered);
   if (dev->info.gfx_level >= GFX10)
      config->rsrc1 |= S_00B848_WGP_MODE(config->wgp_mode);
   config->rsrc2 = S_00B84C_USER_SGPR(shader->num_user_sgprs) |
                  S_00B84C_TGID_X_EN(1) | S_00B84C_TGID_Y_EN(1) | S_00B84C_TGID_Z_EN(1) |
                  S_00B84C_TIDIG_COMP_CNT(2) |
                  S_00B84C_LDS_SIZE(ac_shader_encode_lds_size(config->lds_size, dev->info.gfx_level,
                                                             MESA_SHADER_COMPUTE));
   config->rsrc3 = 0;
   if (dev->info.gfx_level >= GFX10)
      config->rsrc3 = S_00B8A0_SHARED_VGPR_CNT(config->num_shared_vgprs / 8);
   if (dev->info.gfx_level >= GFX11) {
      unsigned prefetch = ac_get_instr_prefetch_size(dev->info.gfx_level,
                                                     dev->info.instr_prefetch_distance, shader->exec_size);
      config->rsrc3 |= dev->info.gfx_level >= GFX12 ? S_00B8A0_INST_PREF_SIZE_GFX12(prefetch) :
                                                   S_00B8A0_INST_PREF_SIZE_GFX11(prefetch);
   }

   unsigned upload_size = ac_align_shader_binary_for_prefetch(dev->info.gfx_level,
                                                              dev->info.instr_prefetch_distance,
                                                              shader->code_size);
   void *code = galloca(dev, upload_size, 256);
   memcpy(code, shader->code, shader->code_size);
   memset((uint8_t *)code + shader->code_size, 0, upload_size - shader->code_size);
   free(shader->code);
   shader->code = code;
   ralloc_free(nir);
   glsl_type_singleton_decref();
   return shader;

fail:
   if (shader) {
      free(shader->code);
      free(shader);
   }
   ralloc_free(nir);
   glsl_type_singleton_decref();
   return NULL;
}
