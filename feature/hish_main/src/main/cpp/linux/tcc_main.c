// Keep the upstream frontend/compiler unchanged. Adapt only application data
// paths and the default ET_EXEC address, after upstream argument parsing (also
// covers @response files). Explicit addresses, objects, DSOs and -run are intact.
#include "libtcc.c"
static int HiSHTCCParseArgs(TCCState* state, int* argc, char*** argv)
{
    const int result=tcc_parse_args(state,argc,argv);
    if(result>=0&&(state->output_type==0||state->output_type==TCC_OUTPUT_EXE)&&!state->has_text_addr){
        state->text_addr=0x60000000;state->has_text_addr=1;
    }
    return result;
}
#undef ONE_SOURCE
#define ONE_SOURCE 0
#define tcc_parse_args HiSHTCCParseArgs
#define main HiSHTCCMain
#include "tcc.c"
#undef main
#undef tcc_parse_args
#undef malloc
#undef calloc
#undef free
int main(int argc,char** argv)
{
    const char* root=getenv("HISH_TOOLCHAIN_ROOT");
    if(!root)return HiSHTCCMain(argc,argv);
    size_t length=strlen(root)+5;char* library=malloc(length);
    char** args=calloc((size_t)argc+3,sizeof(*args));
    if(!library||!args){free(library);free(args);return 1;}
    snprintf(library,length,"%s/lib",root);
    args[0]=argv[0];args[1]="-B";args[2]=library;
    for(int i=1;i<argc;i++)args[i+2]=argv[i];
    int result=HiSHTCCMain(argc+2,args);free(args);free(library);return result;
}
