#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "quickjs.h"

static char *normalize(JSContext *context, const char *base, const char *name, void *opaque) {
    (void) base;
    (void) opaque;
    return js_strdup(context, name);
}

static int module_init(JSContext *context, JSModuleDef *module) {
    JS_SetModuleExport(context, module, "default", JS_UNDEFINED);
    return 0;
}

static JSModuleDef *load(JSContext *context, const char *name, void *opaque) {
    JSModuleDef *module;
    (void) opaque;
    if (strcmp(name, "global") && strcmp(name, "fs")) return NULL;
    module = JS_NewCModule(context, name, module_init);
    if (module) JS_AddModuleExport(context, module, "default");
    return module;
}

int main(int count, char **arguments) {
    FILE *input;
    FILE *output;
    long size;
    char *source;
    uint8_t *bytecode;
    size_t bytecode_size;
    JSRuntime *runtime;
    JSContext *context;
    JSValue value;
    const char *name;
    if (count != 3 && (count != 4 || strcmp(arguments[3], "module"))) return 2;
    input = fopen(arguments[1], "rb");
    if (!input) return 1;
    if (fseek(input, 0, SEEK_END) || (size = ftell(input)) < 0 || fseek(input, 0, SEEK_SET))
        return 1;
    source = malloc((size_t) size + 1);
    if (!source || fread(source, 1, (size_t) size, input) != (size_t) size) return 1;
    fclose(input);
    source[size] = '\0';
    runtime = JS_NewRuntime();
    JS_SetModuleLoaderFunc(runtime, normalize, load, NULL);
    context = JS_NewContext(runtime);
    name = strrchr(arguments[1], '/');
    value = JS_Eval(context, source, (size_t) size, name ? name + 1 : arguments[1],
                    (count == 4 ? JS_EVAL_TYPE_MODULE : JS_EVAL_TYPE_GLOBAL) |
                    JS_EVAL_FLAG_COMPILE_ONLY);
    free(source);
    if (JS_IsException(value)) {
        JSValue exception = JS_GetException(context);
        const char *message = JS_ToCString(context, exception);
        if (message) {
            fputs(message, stderr);
            fputc('\n', stderr);
            JS_FreeCString(context, message);
        }
        JS_FreeValue(context, exception);
        return 1;
    }
    bytecode = JS_WriteObject(context, &bytecode_size, value, JS_WRITE_OBJ_BYTECODE);
    JS_FreeValue(context, value);
    if (!bytecode) return 1;
    output = fopen(arguments[2], "wb");
    if (!output || fwrite(bytecode, 1, bytecode_size, output) != bytecode_size) return 1;
    fclose(output);
    js_free(context, bytecode);
    JS_FreeContext(context);
    JS_FreeRuntime(runtime);
    return 0;
}