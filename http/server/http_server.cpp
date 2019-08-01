#include "http_server.h"

#include "h.h"
#include "hmain.h"
#include "hloop.h"
#include "hbuf.h"

#include "FileCache.h"
#include "HttpParser.h"
#include "HttpHandler.h"

#define RECV_BUFSIZE    4096
#define SEND_BUFSIZE    4096

static HttpService  s_default_service;
static FileCache    s_filecache;

static void master_init(void* userdata) {
#ifdef OS_UNIX
    char proctitle[256] = {0};
    snprintf(proctitle, sizeof(proctitle), "%s: master process", g_main_ctx.program_name);
    setproctitle(proctitle);
#endif
}

static void master_proc(void* userdata) {
    while(1) sleep(1);
}

static void worker_init(void* userdata) {
#ifdef OS_UNIX
    char proctitle[256] = {0};
    snprintf(proctitle, sizeof(proctitle), "%s: worker process", g_main_ctx.program_name);
    setproctitle(proctitle);
    signal(SIGNAL_RELOAD, signal_handler);
#endif
}

static void on_read(hio_t* io, void* buf, int readbytes) {
    //printf("on_read fd=%d readbytes=%d\n", io->fd, readbytes);
    HttpHandler* handler = (HttpHandler*)io->userdata;
    HttpParser* parser = &handler->parser;
    // recv -> HttpParser -> HttpRequest -> handle_request -> HttpResponse -> send
    int nparse = parser->execute((char*)buf, readbytes);
    if (nparse != readbytes || parser->get_errno() != HPE_OK) {
        hloge("[%s:%d] http parser error: %s", handler->srcip, handler->srcport, http_errno_description(parser->get_errno()));
        hclose(io);
        return;
    }
    if (parser->get_state() == HP_MESSAGE_COMPLETE) {
        handler->handle_request();
        // prepare header body
        time_t tt;
        time(&tt);
        char c_str[256] = {0};
        strftime(c_str, sizeof(c_str), "%a, %d %b %Y %H:%M:%S GMT", gmtime(&tt));
        handler->res.headers["Date"] = c_str;
        std::string header = handler->res.dump(true, false);
        const char* body = NULL;
        int content_length = 0;
        if (handler->fc) {
            body = (const char*)handler->fc->filebuf.base;
            content_length = handler->fc->filebuf.len;
        }
        else {
            body = handler->res.body.c_str();
            content_length = handler->res.body.size();
        }
        bool send_in_one_packet;
        if (content_length > (1<<20)) {
            send_in_one_packet = false;
        }
        else {
            send_in_one_packet = true;
            if (content_length > 0) {
                header.insert(header.size(), body, content_length);
            }
        }
        // send header/body
        hwrite(io->loop, io->fd, header.c_str(), header.size());
        if (!send_in_one_packet) {
            // send body
            hwrite(io->loop, io->fd, body, content_length);
        }
        hlogi("[%s:%d][%s %s]=>[%d %s]",
            handler->srcip, handler->srcport,
            http_method_str(handler->req.method), handler->req.url.c_str(),
            handler->res.status_code, http_status_str(handler->res.status_code));
        // Connection: Keep-Alive
        bool keep_alive = false;
        auto iter = handler->req.headers.find("connection");
        if (iter != handler->req.headers.end()) {
            if (stricmp(iter->second.c_str(), "keep-alive") == 0) {
                keep_alive = true;
            }
        }
        if (keep_alive) {
            handler->init();
        }
        else {
            hclose(io);
        }
    }
}

static void on_close(hio_t* io) {
    HttpHandler* handler = (HttpHandler*)io->userdata;
    if (handler) {
        delete handler;
        io->userdata = NULL;
    }
}

static void on_accept(hio_t* io, int connfd) {
    //printf("on_accept listenfd=%d connfd=%d\n", io->fd, connfd);
    //struct sockaddr_in* localaddr = (struct sockaddr_in*)io->localaddr;
    struct sockaddr_in* peeraddr = (struct sockaddr_in*)io->peeraddr;
    //char localip[64];
    //char peerip[64];
    //inet_ntop(localaddr->sin_family, &localaddr->sin_addr, localip, sizeof(localip));
    //inet_ntop(peeraddr->sin_family, &peeraddr->sin_addr, peerip, sizeof(peerip));
    //printd("accept listenfd=%d connfd=%d [%s:%d] <= [%s:%d]\n", io->fd, connfd,
            //localip, ntohs(localaddr->sin_port),
            //peerip, ntohs(peeraddr->sin_port));
    // new HttpHandler
    // delete on_close
    HttpHandler* handler = new HttpHandler;
    handler->service = (HttpService*)io->userdata;
    handler->files = &s_filecache;
    inet_ntop(peeraddr->sin_family, &peeraddr->sin_addr, handler->srcip, sizeof(handler->srcip));
    handler->srcport = ntohs(peeraddr->sin_port);

    nonblocking(connfd);
    HBuf* buf = (HBuf*)io->loop->userdata;
    hio_t* connio = hread(io->loop, connfd, buf->base, buf->len, on_read);
    connio->close_cb = on_close;
    connio->userdata = handler;
}

void handle_cached_files(htimer_t* timer) {
    FileCache* pfc = (FileCache*)timer->userdata;
    if (pfc == NULL) {
        htimer_del(timer);
        return;
    }
    file_cache_t* fc = NULL;
    time_t tt;
    time(&tt);
    auto iter = pfc->cached_files.begin();
    while (iter != pfc->cached_files.end()) {
        fc = iter->second;
        if (tt - fc->stat_time > pfc->file_cached_time) {
            delete fc;
            iter = pfc->cached_files.erase(iter);
            continue;
        }
        ++iter;
    }
}

void fflush_log(hidle_t* idle) {
    hlog_fflush();
}

static void worker_proc(void* userdata) {
    http_server_t* server = (http_server_t*)userdata;
    int listenfd = server->listenfd;
    hloop_t loop;
    hloop_init(&loop);
    // one loop one readbuf.
    HBuf readbuf;
    readbuf.resize(RECV_BUFSIZE);
    loop.userdata = &readbuf;
    hio_t* listenio = haccept(&loop, listenfd, on_accept);
    listenio->userdata = server->service;
    // fflush logfile when idle
    hlog_set_fflush(0);
    hidle_add(&loop, fflush_log, INFINITE);
    // timer handle_cached_files
    htimer_t* timer = htimer_add(&loop, handle_cached_files, s_filecache.file_cached_time*1000);
    timer->userdata = &s_filecache;
    hloop_run(&loop);
}

int http_server_run(http_server_t* server, int wait) {
    // worker_processes
    if (server->worker_processes != 0 && g_worker_processes_num != 0 && g_worker_processes != NULL) {
        return ERR_OVER_LIMIT;
    }
    // service
    if (server->service == NULL) {
        server->service = &s_default_service;
    }
    // port
    server->listenfd = Listen(server->port);
    if (server->listenfd < 0) return server->listenfd;

#ifdef OS_WIN
    if (server->worker_processes > 1) {
        server->worker_processes = 1;
    }
#endif

    if (server->worker_processes == 0) {
        worker_proc(server);
    }
    else {
        // master-workers processes
        g_worker_processes_num = server->worker_processes;
        int bytes = g_worker_processes_num * sizeof(proc_ctx_t);
        g_worker_processes = (proc_ctx_t*)malloc(bytes);
        if (g_worker_processes == NULL) {
            perror("malloc");
            abort();
        }
        memset(g_worker_processes, 0, bytes);
        for (int i = 0; i < g_worker_processes_num; ++i) {
            proc_ctx_t* ctx = g_worker_processes + i;
            ctx->init = worker_init;
            ctx->init_userdata = NULL;
            ctx->proc = worker_proc;
            ctx->proc_userdata = server;
            spawn_proc(ctx);
        }
    }

    if (wait) {
        master_init(NULL);
        master_proc(NULL);
    }
    return 0;
}