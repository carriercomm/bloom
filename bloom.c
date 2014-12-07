#include <memory.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <sys/queue.h>
#include <evhttp.h>
#include <limits.h>
#include <openssl/sha.h>

// Bloom-filter parameters
#define hashpart 33
#define m ( 1L << hashpart )
#define k 10

// Network parameters
#define BIND_ADDRESS "0.0.0.0"
#define BIND_PORT 8888

// Internal definitions
#define MIME_TYPE "Content-Type", "text/html; charset=UTF-8"
#define MEGA (1<<20)
#define STR_MAX 1024
#define BITS_PER_CELL (sizeof(bloom_cell) * CHAR_BIT)
const char miss_response[]  = "MISSING\n";
const char hit_response[]   = "PRESENT\n";
const char added_response[] = "ADDED\n";

typedef unsigned long bloom_cell;
bloom_cell *Bloom = NULL;
unsigned char hashbuf[SHA384_DIGEST_LENGTH];
bloom_cell Ki[k];

//Hasher
bloom_cell *Hashes(const char* bytes)
{
    SHA384(bytes,  strnlen(bytes, STR_MAX), hashbuf);

    int bit, i, j, n=0;
    for (i=0; i < k; i++) {
        bloom_cell curr_key=0;
        for (j=0; j<hashpart; j++,n++) {
            bit = (hashbuf[n / CHAR_BIT] & ((unsigned char)1 << ((CHAR_BIT - 1) - (n % CHAR_BIT)))) !=0 ? 1 : 0;
            curr_key = (curr_key << 1) | bit;
        }
        Ki[i] = curr_key;
    }
    return Ki;
}

//Bloom operations
bool GetBit(bloom_cell *bv, size_t n)
{
    return (bv[n / BITS_PER_CELL] & ((bloom_cell)1 << ((BITS_PER_CELL - 1) - (n % BITS_PER_CELL)))) != 0;
}

void JumpBit(bloom_cell *bv, size_t n)
{
    bv[n / BITS_PER_CELL] |= ((bloom_cell)1 << ((BITS_PER_CELL - 1) - (n % BITS_PER_CELL )) );
}

//URI (commands) handlers
const char *CmdAddHandler(bloom_cell *bloom, const bloom_cell *hashes)
{
    int i;
    for (i=0; i<k; i++)
        JumpBit(bloom, hashes[i]);
    return added_response;
}

const char *CmdCheckHandler(bloom_cell *bloom, const bloom_cell *hashes)
{
    int i;
    for (i=0; i<k; i++)
        if (!GetBit(bloom, hashes[i]))
            return miss_response;
    return hit_response;
}

const char *CmdCheckThenAddHandler(bloom_cell *bloom, const bloom_cell *hashes)
{
    bool present = true;
    int i;
    for (i=0; i<k; i++)
        if (!GetBit(bloom, hashes[i])) {
            present = false;
            break;
        }
    if (!present)
        for (i=0; i<k; i++)
            JumpBit(bloom, hashes[i]);
    return present ? hit_response : miss_response;
}

void* HandlerTable[][2] = {
{"/add",            CmdAddHandler           },
{"/check",          CmdCheckHandler         },
{"/checkthenadd",   CmdCheckThenAddHandler  },
};

//Main
int main()
{
    fprintf(stderr, "Allocating arena with size %.2f MBytes ...\n", (float)m / CHAR_BIT / MEGA);
    Bloom = malloc( (m + ( CHAR_BIT - 1)) / CHAR_BIT ); // Ceil byte length: bytes = bits + 7 / 8

    if (!event_init())
    {
        fputs("Failed to init libevent.", stderr);
        return -1;
    }
    struct evhttp *Server = evhttp_start(BIND_ADDRESS, BIND_PORT);
    if (!Server)
    {
        fputs("Failed to init HTTP-server.", stderr);
        return -1;
    }

    void OnReq(struct evhttp_request *req, void *arg)
    {
        struct evbuffer *OutBuf = evhttp_request_get_output_buffer(req);
        if (!OutBuf) {
            evhttp_send_reply(req, HTTP_BADREQUEST, "Bad Request", OutBuf);
            return;
        }
        struct evkeyvalq *Headers = evhttp_request_get_output_headers(req);
        if (!Headers) {
            evhttp_send_reply(req, HTTP_INTERNAL, "Internal Error", OutBuf);
            return;
        }
        const struct evhttp_uri *HTTPURI =  evhttp_request_get_evhttp_uri(req);
        if (!HTTPURI) {
            evhttp_send_reply(req, HTTP_BADREQUEST, "Bad Request", OutBuf);
            return;
        }
        const char *path =  evhttp_uri_get_path(HTTPURI);
        if (!path) {
            evhttp_send_reply(req, HTTP_BADREQUEST, "Bad Request", OutBuf);
        }
        const char *query_string = evhttp_uri_get_query(HTTPURI);
        if (!query_string) {
            evhttp_send_reply(req, HTTP_BADREQUEST, "Element Required", OutBuf);
            return;
        }
        struct evkeyvalq params;
        evhttp_parse_query_str(query_string, &params);
        const char *element = evhttp_find_header(&params, "e");
        if (!element) {
            evhttp_clear_headers(&params);
            evhttp_send_reply(req, HTTP_BADREQUEST, "Element Required", OutBuf);
            return;
        }

        int i;
        const char* (*Operation)(bloom_cell *, bloom_cell *) = NULL;
        for (i=0; i< sizeof HandlerTable/ sizeof HandlerTable[0] ; i++)
            if (strncmp(HandlerTable[i][0], path, STR_MAX) == 0) {
                Operation = HandlerTable[i][1];
                break;
            }
        if (!Operation) {
            evhttp_clear_headers(&params);
            evhttp_send_reply(req, HTTP_NOTFOUND, "Not Found", OutBuf);
            return;
        }

        const char *response = Operation(Bloom, Hashes(element));

        evhttp_add_header(Headers, MIME_TYPE);
        evbuffer_add_printf(OutBuf, response);
        evhttp_send_reply(req, HTTP_OK, "OK", OutBuf);
        evhttp_clear_headers(&params);
    };
    evhttp_set_gencb(Server, OnReq, 0);

    if (event_dispatch() == -1)
    {
        fputs("Failed to run message loop.", stderr);
        return -1;
    }

    evhttp_free(Server);
    free(Bloom);
    return 0;
}
