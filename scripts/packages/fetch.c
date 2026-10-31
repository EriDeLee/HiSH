// Small musl process using the pinned libapk/libfetch and OpenSSL ABIs.
// libfetch performs redirects/TLS, EVP verifies the pinned artifact digest.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
typedef struct fetchIO fetchIO;
extern fetchIO *fetchGetURL(const char *,const char *);
extern ssize_t fetchIO_read(fetchIO *,void *,size_t);
extern void fetchIO_close(fetchIO *);
extern void fetch_check_certificate(int);
extern int fetchTimeout;
extern int SSL_CTX_load_verify_file(void *,const char *);
extern void SSL_CTX_set_cert_verify_callback(void *,int (*)(void *,void *),void *);
extern int X509_verify_cert(void *);
extern int X509_STORE_CTX_get_error(const void *);
extern const char *X509_verify_cert_error_string(long);
static int verify_certificate(void *ctx,void *unused){(void)unused;int rc=X509_verify_cert(ctx);if(rc!=1)fprintf(stderr,"TLS certificate: %s\n",X509_verify_cert_error_string(X509_STORE_CTX_get_error(ctx)));return rc;}
// libfetch prefers its compiled /etc CA filename if that file happens to
// exist. Require our pinned application CA for both libfetch setup paths.
int SSL_CTX_load_verify_locations(void *ctx,const char *file,const char *directory) {
    (void)file;(void)directory;const char *ca=getenv("SSL_CERT_FILE");SSL_CTX_set_cert_verify_callback(ctx,verify_certificate,NULL);int rc=ca?SSL_CTX_load_verify_file(ctx,ca):0;if(rc!=1)fprintf(stderr,"Application CA load failed\n");return rc;
}
int SSL_CTX_set_default_verify_paths(void *ctx) {return SSL_CTX_load_verify_locations(ctx,NULL,NULL);}
extern struct {unsigned category;int code;} fetchLastErrCode;
typedef struct evp_md_ctx_st EVP_MD_CTX;
typedef struct evp_md_st EVP_MD;
extern EVP_MD_CTX *EVP_MD_CTX_new(void);
extern void EVP_MD_CTX_free(EVP_MD_CTX *);
extern const EVP_MD *EVP_sha256(void);
extern const EVP_MD *EVP_sha512(void);
extern int EVP_DigestInit_ex(EVP_MD_CTX *,const EVP_MD *,void *);
extern int EVP_DigestUpdate(EVP_MD_CTX *,const void *,size_t);
extern int EVP_DigestFinal_ex(EVP_MD_CTX *,unsigned char *,unsigned *);
// Registry metadata has no digest known in advance: TLS and hostname checks
// still apply, the size is capped and the caller validates the content.
static int fetch_metadata(const char *url,const char *file) {
    if(strncmp(url,"https://",8)){fprintf(stderr,"HTTPS required\n");return 1;}
    fetchTimeout=30;fetch_check_certificate(1);fetchIO *stream=fetchGetURL(url,"");
    if(!stream){fprintf(stderr,"fetch: category=%u code=%d\n",fetchLastErrCode.category,fetchLastErrCode.code);return 1;}
    int fd=open(file,O_CREAT|O_EXCL|O_WRONLY|O_CLOEXEC,0600),rc=1;size_t total=0;unsigned char buffer[32768];
    if(fd<0){perror("metadata file");fetchIO_close(stream);return 1;}
    for(;;){ssize_t n=fetchIO_read(stream,buffer,sizeof buffer);if(n<0&&errno==EINTR)continue;if(n<0)goto done;if(!n)break;
        total+=n;if(total>4*1024*1024){fprintf(stderr,"metadata size limit\n");goto done;}
        size_t at=0;while(at<(size_t)n){ssize_t w=write(fd,buffer+at,n-at);if(w<0&&errno==EINTR)continue;if(w<=0)goto done;at+=w;}}
    rc=0;
done:
    if(close(fd))rc=1;fetchIO_close(stream);if(rc)unlink(file);return rc;
}
int main(int argc,char **argv) {
    if(argc==4&&!strcmp(argv[1],"--any"))return fetch_metadata(argv[2],argv[3]);
    if(argc!=4||(strlen(argv[3])!=64&&strlen(argv[3])!=128)){fprintf(stderr,"usage: fetch-helper HTTPS_URL|--hash FILE SHA256_OR_SHA512 | --any HTTPS_URL FILE\n");return 1;}
    const unsigned digest_size=strlen(argv[3])/2;
    const int hash_only=!strcmp(argv[1],"--hash");
    if(!hash_only&&strncmp(argv[1],"https://",8)){fprintf(stderr,"HTTPS required\n");return 1;}
    EVP_MD_CTX *ctx=EVP_MD_CTX_new();if(!ctx||EVP_DigestInit_ex(ctx,digest_size==32?EVP_sha256():EVP_sha512(),NULL)!=1)return 1;
    fetchIO *stream=NULL;int fd=-1,rc=1;size_t total=0;unsigned char buffer[32768],digest[64];unsigned length=0;
    if(hash_only)fd=open(argv[2],O_RDONLY|O_CLOEXEC);
    else {fetchTimeout=30;fetch_check_certificate(1);stream=fetchGetURL(argv[1],"");if(!stream){fprintf(stderr,"fetch: category=%u code=%d\n",fetchLastErrCode.category,fetchLastErrCode.code);goto done;}fd=open(argv[2],O_CREAT|O_EXCL|O_WRONLY|O_CLOEXEC,0600);}
    if(fd<0){perror("artifact file");goto done;}
    for(;;){ssize_t n=hash_only?read(fd,buffer,sizeof buffer):fetchIO_read(stream,buffer,sizeof buffer);if(n<0&&errno==EINTR)continue;if(n<0)goto done;if(!n)break;
        total+=n;if(total>64*1024*1024||EVP_DigestUpdate(ctx,buffer,n)!=1)goto done;
        if(!hash_only){size_t at=0;while(at<(size_t)n){ssize_t written=write(fd,buffer+at,n-at);if(written<0&&errno==EINTR)continue;if(written<=0)goto done;at+=written;}}
    }
    if(EVP_DigestFinal_ex(ctx,digest,&length)!=1||length!=digest_size)goto done;
    char hex[129];for(unsigned i=0;i<digest_size;i++)snprintf(hex+i*2,3,"%02x",digest[i]);
    if(strcmp(hex,argv[3])){fprintf(stderr,"artifact SHA%u mismatch\n",digest_size*8);goto done;}
    printf("Verified SHA%u %s (%zu bytes)\n",digest_size*8,hex,total);rc=0;
done:
    if(fd>=0&&close(fd))rc=1;if(stream)fetchIO_close(stream);EVP_MD_CTX_free(ctx);if(rc&&!hash_only)unlink(argv[2]);return rc;
}
