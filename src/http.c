#include "http.h"
#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

typedef enum _parser_state {
    PARSER_STATE_BAD_REQUEST = -1,
    PARSER_STATE_COMPLETE = 0,
    PARSER_STATE_METHOD,
    PARSER_STATE_PATH,
    PARSER_STATE_QUERY_STR,
    PARSER_STATE_VERSION,
    PARSER_STATE_HEADER_NAME,
    PARSER_STATE_HEADER_COLON,
    PARSER_STATE_HEADER_VALUE,
    PARSER_STATE_HEADER_CR,
    PARSER_STATE_HEADER_LF,
    PARSER_STATE_HEADER_COMPLETE_CR,
} parser_state_e;


typedef void (*http_header_callback)(request_t *req, http_header_t *header);

typedef struct _header_command {
    char                   *lower_name;
    http_header_callback   callback;
} header_command_t;

static struct hsearch_data std_headers_hash;
static int std_headers_hash_initialized = 0;

static http_version_e _resolve_http_version(const char* version_str);
static int init_std_headers_hash();
static void handle_common_header(request_t *req, int header_index);
static void strlowercase(const char *src, char *dst, size_t n);

request_t* request_create() {
    request_t  *req;
    req = (request_t*) calloc(1, sizeof(request_t));
    if (req == NULL) {
        fprintf(stderr, "Unable to malloc\n");
        return NULL;
    }
    bzero(req, sizeof(request_t));
    if (hcreate_r(MAX_HEADER_SIZE, &req->_header_hash) == 0) {
        perror("Error creating header hash table");
        free(req);
        return NULL;
    }
    return req;
}

int request_destroy(request_t *req) {
    hdestroy_r(&req->_header_hash);
    free(req);
    return 0;
}

const char*  request_get_header(request_t *request, const char *header_name) {
    ENTRY          item, *ret;
    char           header_name_lower[64];

    strlowercase(header_name, header_name_lower, 64);
    item.key = header_name_lower;

    if (hsearch_r(item, FIND, &ret, &request->_header_hash) == 0) {
        return NULL;
    }

    return ((http_header_t*) ret->data)->value;
}

#define START_NEW_TOKEN(tok, req)                       \
    (tok = (req)->_buffer + (req)->_buf_idx)

#define FILL_NEXT_CHAR(req, ch)                         \
    ((req)->_buffer[req->_buf_idx++] = (ch))

#define FINISH_CUR_TOKEN(req)                           \
    ((req)->_buffer[(req)->_buf_idx++] = '\0' )

#define EXPECT_CHAR(state, ch, expected_ch, next_state) \
    if ((ch) == (expected_ch)) {                        \
        state = (next_state);                           \
    } else {                                            \
        state = PARSER_STATE_BAD_REQUEST;               \
    }


int request_parse_headers(request_t *req,
                          const char *data,
                          const size_t data_len,
                          size_t *consumed) {
    printf("Parsing HTTP request\n");
    http_version_e      ver;
    int                 i, rc;
    char                ch;
    char                *cur_token = req->_buffer;
    parser_state_e      state = PARSER_STATE_METHOD;

    req->_buffer[0] = '\0';
    req->_buf_idx = 0;
    req->header_count = 0;

    if (std_headers_hash_initialized == 0) {
        init_std_headers_hash();
    }

    for (i = 0; i < data_len;){

        if (state == PARSER_STATE_COMPLETE ||
             state == PARSER_STATE_BAD_REQUEST) {
            break;
        } 
        ch = data[i++];

        switch(state) {

            case PARSER_STATE_METHOD:
                if (ch == ' ') {
                    FINISH_CUR_TOKEN(req);
                    req->method = cur_token;
                    state = PARSER_STATE_PATH;
                    START_NEW_TOKEN(cur_token, req);
                } else if (ch < 'A' || ch > 'Z') {
                    state = PARSER_STATE_BAD_REQUEST;
                } else {
                    FILL_NEXT_CHAR(req, ch);
                }
                break;

            case PARSER_STATE_PATH:
                if (ch == '?') {
                    FINISH_CUR_TOKEN(req);
                    req->path = cur_token;
                    state = PARSER_STATE_QUERY_STR;
                    START_NEW_TOKEN(cur_token, req);
                } else if (ch == ' ') {
                    FINISH_CUR_TOKEN(req);
                    req->path = cur_token;
                    state = PARSER_STATE_VERSION;
                    START_NEW_TOKEN(cur_token, req);
                } else {
                    FILL_NEXT_CHAR(req, ch);
                }
                break;

            case PARSER_STATE_QUERY_STR:
                if (ch == ' ') {
                    FINISH_CUR_TOKEN(req);
                    req->query_str = cur_token;
                    state = PARSER_STATE_VERSION;
                    START_NEW_TOKEN(cur_token, req);
                } else {
                    FILL_NEXT_CHAR(req, ch);
                }
                break;

            case PARSER_STATE_VERSION:
                switch (ch) {
                    // For HTTP part in the request line, e.g. GET / HTTP/1.1
                    case 'H':
                    case 'T':
                    case 'P':

                    // Currently only 0.9, 1.0 and 1.1 are supported.
                    case '0':
                    case '1':
                    case '9':
                    case '.':
                        FILL_NEXT_CHAR(req, ch);
                        break;

                    case '/':
                        FINISH_CUR_TOKEN(req);
                        if (strcmp("HTTP", cur_token) != 0) {
                            state = PARSER_STATE_BAD_REQUEST;
                            break;
                        }
                        START_NEW_TOKEN(cur_token, req);
                        break;

                    case '\r':
                        FINISH_CUR_TOKEN(req);
                        ver = _resolve_http_version(cur_token);
                        if (ver == HTTP_VERSION_UNKNOW) {
                            state = PARSER_STATE_BAD_REQUEST;
                            break;
                        }
                        req->version = ver;
                        state = PARSER_STATE_HEADER_CR;
                        START_NEW_TOKEN(cur_token, req);
                        break;
                }
                break;

            case PARSER_STATE_HEADER_NAME:
                if (ch == ':') {
                    FINISH_CUR_TOKEN(req);
                    req->headers[req->header_count].name = cur_token;
                    state = PARSER_STATE_HEADER_COLON;
                    START_NEW_TOKEN(cur_token, req);
                } else {
                    FILL_NEXT_CHAR(req, ch);
                }
                break;

            case PARSER_STATE_HEADER_COLON:
                EXPECT_CHAR(state, ch, ' ', PARSER_STATE_HEADER_VALUE);
                break;

            case PARSER_STATE_HEADER_VALUE:
                if (ch == '\r') {
                    FINISH_CUR_TOKEN(req);
                    req->headers[req->header_count].value = cur_token;
                    handle_common_header(req, req->header_count);
                    req->header_count++;
                    state = PARSER_STATE_HEADER_CR;
                    START_NEW_TOKEN(cur_token, req);
                } else {
                    FILL_NEXT_CHAR(req, ch);
                }
                break;

            case PARSER_STATE_HEADER_CR:
                EXPECT_CHAR(state, ch, '\n', PARSER_STATE_HEADER_LF);
                break;

            case PARSER_STATE_HEADER_LF:
                // Another CR after a header LF, meanning the header end is met.
                if (ch == '\r') { 
                    state = PARSER_STATE_HEADER_COMPLETE_CR; 
                } else { 
                    state = PARSER_STATE_HEADER_NAME; 
                    FILL_NEXT_CHAR(req, ch);
                }
                break;

            case PARSER_STATE_HEADER_COMPLETE_CR:
                EXPECT_CHAR(state, ch, '\n', PARSER_STATE_COMPLETE);
                break;

            default:
                fprintf(stderr, "Unexpected state: %d\n", state);
                break;

        }
    }

    *consumed = i;

    switch (state) {
        case PARSER_STATE_COMPLETE:
            rc = STATUS_COMPLETE;
            break;

        case PARSER_STATE_BAD_REQUEST:
            rc = STATUS_ERROR;
            break;

        default:
            rc = STATUS_CONTINUE;
            break;
    }

    return rc;
}

static http_version_e _resolve_http_version(const char* version_str) {
    if (strcmp(version_str, "1.1") == 0) {
        return HTTP_VERSION_1_1;
    } else if (strcmp(version_str, "1.0") == 0) {
        return HTTP_VERSION_1_0;
    } else if (strcmp(version_str, "0.9") == 0) {
        return HTTP_VERSION_0_9;
    }

    return HTTP_VERSION_UNKNOW;
}

static void _handle_content_len(request_t *req, http_header_t *header) {
    size_t   content_length;
    printf("Setting content length\n");

    content_length = (size_t) atol(header->value);
    req->content_length = content_length;
}

static void _handle_host(request_t *req, http_header_t *header) {
    printf("Setting host\n");
    req->host = header->value;
}

static void _handle_connection(request_t *req, http_header_t *header) {
    printf("Setting connection option\n");
    if (strcasecmp("keep-alive", header->value) == 0) {
        req->connection = CONN_KEEP_ALIVE;
    }
}

static header_command_t std_headers[] = {
    { "accept", NULL },
    { "accept-charset", NULL },
    { "accept-datetime", NULL },
    { "accept-encoding", NULL },
    { "accept-language", NULL },
    { "accept-ranges", NULL },
    { "access-control-allow-origin", NULL },
    { "age", NULL },
    { "allow", NULL },
    { "authorization", NULL },
    { "cache-control", NULL },
    { "connection", _handle_connection },
    { "content-disposition", NULL },
    { "content-encoding", NULL },
    { "content-language", NULL },
    { "content-length", _handle_content_len },
    { "content-location", NULL },
    { "content-md5", NULL },
    { "content-range", NULL },
    { "content-security-policy", NULL },
    { "content-type", NULL },
    { "cookie", NULL },
    { "dnt", NULL },
    { "date", NULL },
    { "etag", NULL },
    { "expect", NULL },
    { "expires", NULL },
    { "from", NULL },
    { "front-end-https", NULL },
    { "host", _handle_host },
    { "if-match", NULL },
    { "if-modified-since", NULL },
    { "if-none-match", NULL },
    { "if-range", NULL },
    { "if-unmodified-since", NULL },
    { "last-modified", NULL },
    { "link", NULL },
    { "location", NULL },
    { "max-forwards", NULL },
    { "origin", NULL },
    { "p3p", NULL },
    { "pragma", NULL },
    { "proxy-authenticate", NULL },
    { "proxy-authorization", NULL },
    { "proxy-connection", NULL },
    { "range", NULL },
    { "referer", NULL },
    { "refresh", NULL },
    { "retry-after", NULL },
    { "server", NULL },
    { "set-cookie", NULL },
    { "status", NULL },
    { "strict-transport-security", NULL },
    { "te", NULL },
    { "trailer", NULL },
    { "transfer-encoding", NULL },
    { "upgrade", NULL },
    { "user-agent", NULL },
    { "vary", NULL },
    { "via", NULL },
    { "www-authenticate", NULL },
    { "warning", NULL },
    { "x-att-deviceid", NULL },
    { "x-content-security-policy", NULL },
    { "x-content-type-options", NULL },
    { "x-forwarded-for", NULL },
    { "x-forwarded-proto", NULL },
    { "x-frame-options", NULL },
    { "x-powered-by", NULL },
    { "x-requested-with", NULL },
    { "x-wap-profile", NULL },
    { "x-webkit-csp", NULL },
    { "x-xss-protection", NULL },
    { "x-ua-compatible", NULL },
};

static int init_std_headers_hash() {
    int i;
    size_t size;
    ENTRY item, *ret;

    size = sizeof(std_headers)/sizeof(std_headers[0]);

    bzero(&std_headers_hash, sizeof(struct hsearch_data));
    if (hcreate_r(sizeof(std_headers) * 2, &std_headers_hash) == 0) {
        perror("Error creating standard headers hash");
        return -1;
    }
    for (i = 0; i < size; i++) {
        item.key = std_headers[i].lower_name;
        item.data = std_headers[i].callback;
        if (hsearch_r(item, ENTER, &ret, &std_headers_hash) == 0) {
            fprintf(stderr, "Error entering standard header %s to hash\n", item.key);
        }
    }
    std_headers_hash_initialized = 1;
    return 0;
}

__inline__ static void strlowercase(const char *src, char *dst, size_t n) {
    int i;
    const char *p;
    for (i = 0, p = src;
         i < n && *p != '\0';
         i++, p++) {
        dst[i] = tolower(*p);
    }
    dst[i] = '\0';
}

static void handle_common_header(request_t *req, int header_index) {
    ENTRY           ent, *ret;
    http_header_t  *header;
    char            header_lowercase[64];
    http_header_callback callback = NULL;

    header = req->headers + header_index;
    strlowercase(header->name, header_lowercase, 64);
    ent.key = header_lowercase;
    if (hsearch_r(ent, FIND, &ret, &std_headers_hash) == 0) {
        return;
    }

    callback = (http_header_callback) ret->data;
    if (callback != NULL) {
        callback(req, header);
    }

    ent.key = ret->key;
    ent.data = header;

    if (hsearch_r(ent, ENTER, &ret, &req->_header_hash) != 0) {
        printf("Successfully add known header %s to header hash\n", ent.key);
    }
}

response_t* response_create() {
    response_t   *resp;

    resp = (response_t*) calloc(1, sizeof(response_t));
    if (resp == NULL) {
        fprintf(stderr, "Error allocating memory");
        return NULL;
    }

    bzero(resp, sizeof(response_t));
    if (hcreate_r(MAX_HEADER_SIZE, &resp->_header_hash) == 0) {
        perror("Error creating header hash table");
        free(resp);
        return NULL;
    }

    return resp;
}

int response_destroy(response_t *response) {
    hdestroy_r(&response->_header_hash);
    free(response);
    return 0;
}

const char* response_get_header(response_t *response, const char *header_name) {
    ENTRY          item, *ret;
    char           header_name_lower[64];

    strlowercase(header_name, header_name_lower, 64);
    item.key = header_name_lower;

    if (hsearch_r(item, FIND, &ret, &response->_header_hash) == 0) {
        return NULL;
    }

    return ((http_header_t*) ret->data)->value;
}

int response_set_header(response_t *response, char *header_name, char *header_value) {
    ENTRY          ent, *ret;
    http_header_t  *header;
    char           header_lowercase[64];
    size_t         n, len;

    strlowercase(header_name, header_lowercase, 64);
    ent.key = header_lowercase;
    if (hsearch_r(ent, FIND, &ret, &response->_header_hash) != 0) {
        header = (http_header_t*) ret->data;
        header->value = header_value;
        return 0;
    }

    header = response->headers + response->header_count++;
    header->name = header_name;
    header->value = header_value;

    if (hsearch_r(ent, FIND, &ret, &std_headers_hash) != 0) {
        ent.key = ret->key;
    } else {
        len = strlen(header_lowercase) + 1;
        n = MIN(len, MAX_HEADER_SIZE - response->_buf_idx);
        ent.key = strncpy(response->_buffer + response->_buf_idx,
                          header_lowercase,
                          n);
        response->_buf_idx += n;
    }
    ent.data = header;
    if (hsearch_r(ent, ENTER, &ret, &response->_header_hash) == 0) {
        fprintf(stderr, "Error inputing header to header hash: %s", header_name);
        return -1;
    }
    return 0;
}

int response_write(response_t *response,
                   handler_func next_handler) {
    return 0;
}
