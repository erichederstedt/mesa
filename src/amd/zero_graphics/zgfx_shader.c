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

static unsigned shader_io_size(const struct glsl_type *type, bool bindless)
{
   return glsl_count_attribute_slots(type, false);
}

static bool shader_lower_graphics_io(nir_shader *nir, zgfx_shader *shader)
{
   NIR_PASS(_, nir, nir_lower_array_deref_of_vec, nir_var_shader_in | nir_var_shader_out, NULL,
            nir_lower_direct_array_deref_of_vec_load | nir_lower_indirect_array_deref_of_vec_load |
            nir_lower_direct_array_deref_of_vec_store | nir_lower_indirect_array_deref_of_vec_store);
   nir_assign_io_var_locations(nir, nir_var_shader_in);
   nir_assign_io_var_locations(nir, nir_var_shader_out);
   NIR_PASS(_, nir, nir_lower_io, nir_var_shader_in | nir_var_shader_out, shader_io_size,
            nir_lower_io_lower_64bit_to_32 | nir_lower_io_use_interpolated_input_intrinsics);
   nir->info.io_lowered = true;
   NIR_PASS(_, nir, nir_opt_constant_folding);
   NIR_PASS(_, nir, nir_opt_dce);
   NIR_PASS(_, nir, nir_remove_dead_variables, nir_var_shader_in | nir_var_shader_out, NULL);
   nir_shader_gather_info(nir, nir_shader_get_entrypoint(nir));

   if (nir->info.inputs_read) {
      fprintf(stderr, "zgfx: graphics shader inputs are not supported yet; use physical pointers\n");
      return false;
   }
   unsigned sysval;
   BITSET_FOREACH_SET(sysval, nir->info.system_values_read, SYSTEM_VALUE_MAX) {
      if (nir->info.stage == MESA_SHADER_VERTEX &&
          (sysval == SYSTEM_VALUE_VERTEX_ID_ZERO_BASE || sysval == SYSTEM_VALUE_FIRST_VERTEX))
         continue;
      fprintf(stderr, "zgfx: unsupported graphics system value %u\n", sysval);
      return false;
   }

   if (nir->info.stage == MESA_SHADER_VERTEX) {
      if (!(nir->info.outputs_written & VARYING_BIT_POS) ||
          (nir->info.outputs_written & ~(VARYING_BIT_POS | (BITFIELD64_MASK(32) << VARYING_SLOT_VAR0))) ||
          nir->info.outputs_written_16bit) {
         fprintf(stderr, "zgfx: vertex shaders require position and support location 0-31 outputs\n");
         return false;
      }
      memset(shader->vs_param_offsets, AC_EXP_PARAM_UNDEFINED, sizeof(shader->vs_param_offsets));
      for (unsigned slot = VARYING_SLOT_VAR0; slot <= VARYING_SLOT_VAR31; slot++) {
         if (nir->info.outputs_written & BITFIELD64_BIT(slot))
            shader->vs_param_offsets[slot] = shader->vs_param_exports++;
      }
      shader->spi_shader_pos_format = S_02870C_POS0_EXPORT_FORMAT(V_02870C_SPI_SHADER_4COMP);
      NIR_PASS(_, nir, ac_nir_lower_legacy_vs, shader->dev->info.gfx_level, 0, false,
               shader->vs_param_offsets, shader->vs_param_exports != 0, false, true, false);
   } else {
      if (nir->info.outputs_written != BITFIELD64_BIT(FRAG_RESULT_DATA0)) {
         fprintf(stderr, "zgfx: pixel shaders currently require a single color output at location 0\n");
         return false;
      }
      shader->spi_shader_col_format = V_028714_SPI_SHADER_32_ABGR;
      NIR_PASS(_, nir, ac_nir_lower_ps_late, &(ac_nir_lower_ps_late_options){
         .gfx_level = shader->dev->info.gfx_level,
         .use_aco = true,
         .uses_discard = nir->info.fs.uses_discard,
         .spi_shader_col_format = shader->spi_shader_col_format,
      });
   }
   return true;
}

PUBLIC zgfx_shader *shader_create(zgfx_device *dev, uint8_t *spriv_bytes,
                                  uint64_t spriv_size, zgfx_shader_type shader_type, char *entry)
{
   if (!dev || !spriv_bytes || spriv_size < 20 || spriv_size > SIZE_MAX ||
       spriv_size % 4 || !entry || !*entry ||
       (shader_type != ZGFX_SHADER_COMPUTE && shader_type != ZGFX_SHADER_VERTEX &&
        shader_type != ZGFX_SHADER_PIXEL)) {
      fprintf(stderr, "zgfx: shader_create requires SPIR-V, a supported stage and an entry point\n");
      return NULL;
   }
   if (!aco_is_gpu_supported(&dev->info)) {
      fprintf(stderr, "zgfx: GPU is not supported by ACO\n");
      return NULL;
   }
   if (shader_type != ZGFX_SHADER_COMPUTE && dev->info.gfx_level >= GFX11) {
      fprintf(stderr, "zgfx: graphics shaders currently require GFX6-GFX10.3\n");
      return NULL;
   }
   mesa_shader_stage stage = shader_type == ZGFX_SHADER_COMPUTE ? MESA_SHADER_COMPUTE :
                             shader_type == ZGFX_SHADER_VERTEX ? MESA_SHADER_VERTEX : MESA_SHADER_FRAGMENT;
   enum ac_hw_stage hw_stage = shader_type == ZGFX_SHADER_COMPUTE ? AC_HW_COMPUTE_SHADER :
                              shader_type == ZGFX_SHADER_VERTEX ? AC_HW_VERTEX_SHADER : AC_HW_PIXEL_SHADER;

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
   nir_shader *nir = spirv_to_nir(words, spriv_size / 4, NULL, stage,
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
   for (unsigned i = 0; stage == MESA_SHADER_COMPUTE && i < 3; i++) {
      if (!nir->info.workgroup_size[i] || nir->info.workgroup_size[i] > 1024)
         goto fail;
      workgroup_size *= nir->info.workgroup_size[i];
   }
   if (stage == MESA_SHADER_COMPUTE && (nir->info.workgroup_size_variable || workgroup_size > 1024)) {
      fprintf(stderr, "zgfx: unsupported compute workgroup size\n");
      goto fail;
   }

   shader = calloc(1, sizeof(*shader));
   if (!shader)
      goto fail;
   shader->dev = dev;
   shader->type = shader_type;
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
   if (stage != MESA_SHADER_COMPUTE)
      NIR_PASS(_, nir, nir_lower_io_vars_to_temporaries, nir_shader_get_entrypoint(nir),
               nir_var_shader_in | nir_var_shader_out);
   NIR_PASS(_, nir, nir_lower_global_vars_to_local);
   NIR_PASS(_, nir, nir_lower_var_copies);
   NIR_PASS(_, nir, nir_lower_memcpy);
   NIR_PASS(_, nir, nir_lower_vars_to_ssa);
   NIR_PASS(_, nir, nir_lower_system_values);
   if (stage == MESA_SHADER_COMPUTE)
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
   if (stage != MESA_SHADER_COMPUTE && !shader_lower_graphics_io(nir, shader))
      goto fail;

   struct ac_shader_args *args = &shader->args;
   ac_add_arg(args, AC_ARG_SGPR, 2, AC_ARG_VALUE, &shader->argument_ptr);
   if (stage == MESA_SHADER_VERTEX)
      ac_add_arg(args, AC_ARG_SGPR, 1, AC_ARG_VALUE, &args->base_vertex);
   if (stage == MESA_SHADER_COMPUTE && BITSET_TEST(nir->info.system_values_read, SYSTEM_VALUE_NUM_WORKGROUPS))
      ac_add_arg(args, AC_ARG_SGPR, 3, AC_ARG_VALUE, &args->num_work_groups);
   shader->num_user_sgprs = args->num_sgprs_used;
   if (stage == MESA_SHADER_VERTEX) {
      ac_add_arg(args, AC_ARG_VGPR, 1, AC_ARG_VALUE, &args->vertex_id);
   } else if (stage == MESA_SHADER_FRAGMENT) {
      ac_add_arg(args, AC_ARG_SGPR, 1, AC_ARG_VALUE, &args->prim_mask);
      ac_add_arg(args, AC_ARG_VGPR, 2, AC_ARG_VALUE, &args->persp_center);
   } else {
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
               .hw_stage = hw_stage,
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
      .hw_stage = hw_stage,
      .wave_size = shader->wave_size,
      .workgroup_size = workgroup_size,
      .lds_size = nir->info.shared_size,
      .ps.spi_ps_input_ena = stage == MESA_SHADER_FRAGMENT ? S_0286CC_PERSP_CENTER_ENA(1) : 0,
      .ps.spi_ps_input_addr = stage == MESA_SHADER_FRAGMENT ? S_0286D0_PERSP_CENTER_ENA(1) : 0,
   };
   void *binary = shader;
   aco_compile_shader(&aco_options, &aco_info, 1, &nir, args, shader_build_binary, &binary);
   if (!shader->code)
      goto fail;

   struct ac_shader_config *config = &shader->config;
   unsigned vgpr_granularity = dev->info.compiler_info.wave64_vgpr_encode_granularity;
   unsigned vgprs = MAX2(config->num_vgprs, args->num_vgprs_used);
   unsigned sgprs = MAX2(config->num_sgprs, args->num_sgprs_used);
   if (stage == MESA_SHADER_COMPUTE) {
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
   } else {
      config->rsrc1 = S_00B128_VGPRS(DIV_ROUND_UP(vgprs, vgpr_granularity) - 1) |
                     S_00B128_FLOAT_MODE(config->float_mode) | S_00B128_DX10_CLAMP(1);
      if (dev->info.gfx_level < GFX10)
         config->rsrc1 |= S_00B128_SGPRS(DIV_ROUND_UP(sgprs, 8) - 1);
      else
         config->rsrc1 |= stage == MESA_SHADER_VERTEX ? S_00B128_MEM_ORDERED(config->mem_ordered) :
                                                      S_00B028_MEM_ORDERED(config->mem_ordered);
      config->rsrc2 = S_00B12C_USER_SGPR(shader->num_user_sgprs);
      if (dev->info.gfx_level >= GFX10)
         config->rsrc2 |= stage == MESA_SHADER_VERTEX ? S_00B12C_SHARED_VGPR_CNT(config->num_shared_vgprs / 8) :
                                                      S_00B02C_SHARED_VGPR_CNT(config->num_shared_vgprs / 8);
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
