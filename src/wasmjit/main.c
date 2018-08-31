/* -*-mode:c; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */

/*
  Copyright (c) 2018 Rian Hunter

  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"), to deal
  in the Software without restriction, including without limitation the rights
  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  copies of the Software, and to permit persons to whom the Software is
  furnished to do so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included in all
  copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
  SOFTWARE.
 */

#include <wasmjit/ast.h>
#include <wasmjit/compile.h>
#include <wasmjit/ast_dump.h>
#include <wasmjit/parse.h>
#include <wasmjit/runtime.h>
#include <wasmjit/instantiate.h>
#include <wasmjit/execute.h>
#include <wasmjit/emscripten_runtime.h>
#include <wasmjit/dynamic_emscripten_runtime.h>
#include <wasmjit/elf_relocatable.h>

#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <errno.h>

#include <unistd.h>
#include <fcntl.h>

#include <sys/stat.h>
#include <sys/types.h>

char *load_file(const char *file_name, size_t *size)
{
	FILE *f = NULL;
	char *input = NULL;
	int fd = -1, ret;
	struct stat st;
	size_t rets;

	fd = open(file_name, O_RDONLY);
	if (fd < 0) {
		goto error_exit;
	}

	ret = fstat(fd, &st);
	if (ret < 0) {
		goto error_exit;
	}

	f = fdopen(fd, "r");
	if (!f) {
		goto error_exit;
	}
	fd = -1;

	*size = st.st_size;
	input = malloc(st.st_size);
	if (!input) {
		goto error_exit;
	}

	rets = fread(input, sizeof(char), st.st_size, f);
	if (rets != (size_t) st.st_size) {
		goto error_exit;
	}

	goto success_exit;

 error_exit:
	if (input) {
		free(input);
	}

 success_exit:
	if (f) {
		fclose(f);
	}

	if (fd >= 0) {
		close(fd);
	}

	return input;
}

int init_pstate_user(struct ParseState *pstate, const char *file_name)
{
	size_t size;
	char *buf = load_file(file_name, &size);
	if (!buf)
		return 0;

	return init_pstate(pstate, buf, size);
}

int main(int argc, char *argv[])
{
	int ret;
	struct ParseState pstate;
	struct Module module;
	int dump_module, create_relocatable, opt;
	char error_buffer[0x1000] = {0};

	dump_module =  0;
	create_relocatable =  0;
	while ((opt = getopt(argc, argv, "do")) != -1) {
		switch (opt) {
		case 'o':
			create_relocatable = 1;
			break;
		case 'd':
			dump_module = 1;
			break;
		default:
			return -1;
		}
	}

	if (optind >= argc) {
		printf("Need an input file\n");
		return -1;
	}

	ret = init_pstate_user(&pstate, argv[optind]);
	if (!ret) {
		printf("Error loading file %s\n", strerror(errno));
		return -1;
	}

	ret = read_module(&pstate, &module, NULL, 0);
	if (!ret) {
		printf("Error parsing module\n");
		return -1;
	}

	if (dump_module) {
		uint32_t i;

		for (i = 0; i < module.code_section.n_codes; ++i) {
			uint32_t j;
			struct TypeSectionType *type;
			struct CodeSectionCode *code =
			    &module.code_section.codes[i];

			type =
			    &module.type_section.types[module.function_section.
						       typeidxs[i]];

			printf("Code #%" PRIu32 "\n", i);

			printf("Locals (%" PRIu32 "):\n", code->n_locals);
			for (j = 0; j < code->n_locals; ++j) {
				printf("  %s (%" PRIu32 ")\n",
				       wasmjit_valtype_repr(code->locals[j].
							    valtype),
				       code->locals[j].count);
			}

			printf("Signature: [");
			for (j = 0; j < type->n_inputs; ++j) {
				printf("%s,",
				       wasmjit_valtype_repr(type->
							    input_types[j]));
			}
			printf("] -> [");
			for (j = 0; j < FUNC_TYPE_N_OUTPUTS(type); ++j) {
				printf("%s,",
				       wasmjit_valtype_repr(FUNC_TYPE_OUTPUT_IDX(type, j)));
			}
			printf("]\n");

			printf("Instructions:\n");
			dump_instructions(module.code_section.codes[i].
					  instructions,
					  module.code_section.codes[i].
					  n_instructions, 1);
			printf("\n");
		}

		return 0;
	}

	/* the most basic validation */
	if (module.code_section.n_codes != module.function_section.n_typeidxs) {
		printf("# Functions != # Codes %" PRIu32 " != %" PRIu32 "\n",
		       module.function_section.n_typeidxs,
		       module.code_section.n_codes);
		return -1;
	}

	if (create_relocatable) {
		void *a_out;
		size_t size;

		a_out = wasmjit_output_elf_relocatable("asm", &module, &size);
		write(1, a_out, size);

		return 0;
	}

	{
		struct NamedModule *modules;
		struct ModuleInst *module_inst, *env_module_inst;
		size_t n_modules, i;
		struct FuncInst *main_inst, *stack_alloc_inst;
		struct MemInst *meminst;

		modules = wasmjit_instantiate_emscripten_runtime(&n_modules);

		if (!modules)
			return -1;

		env_module_inst = NULL;
		for (i = 0; i < n_modules; ++i) {
			if (!strcmp(modules[i].name, "env")) {
				env_module_inst = modules[i].module;
				break;
			}
		}

		if (!env_module_inst)
			return -1;

		module_inst = wasmjit_instantiate(&module, n_modules, modules,
						  error_buffer, sizeof(error_buffer));
		if (!module_inst) {
			fprintf(stderr, "Error instantiating module: %s\n",
				error_buffer);
			return -1;
		}

		main_inst = wasmjit_get_export(module_inst, "_main",
					       IMPORT_DESC_TYPE_FUNC).func;
		if (!main_inst) {
			fprintf(stderr, "Couldn't find _main\n");
			return -1;
		}

		stack_alloc_inst = wasmjit_get_export(module_inst, "stackAlloc",
						      IMPORT_DESC_TYPE_FUNC).func;
		if (!stack_alloc_inst) {
			fprintf(stderr, "Couldn't find stackAlloc\n");
			return -1;
		}

		meminst = wasmjit_get_export(env_module_inst, "memory",
					     IMPORT_DESC_TYPE_MEM).mem;
		if (!meminst) {
			fprintf(stderr, "Couldn't find env.memory\n");
			return -1;
		}

		return wasmjit_emscripten_invoke_main(meminst,
						      stack_alloc_inst,
						      main_inst,
						      argc - optind, &argv[optind]);
	}
}
