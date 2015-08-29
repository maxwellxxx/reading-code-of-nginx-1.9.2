
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_event.h>


#if 0
#define NGX_SENDFILE_LIMIT  4096
#endif

/*
 * When DIRECTIO is enabled FreeBSD, Solaris, and MacOSX read directly
 * to an application memory from a device if parameters are aligned
 * to device sector boundary (512 bytes).  They fallback to usual read
 * operation if the parameters are not aligned.
 * Linux allows DIRECTIO only if the parameters are aligned to a filesystem
 * sector boundary, otherwise it returns EINVAL.  The sector size is
 * usually 512 bytes, however, on XFS it may be 4096 bytes.
 */

#define NGX_NONE            1


static ngx_inline ngx_int_t
    ngx_output_chain_as_is(ngx_output_chain_ctx_t *ctx, ngx_buf_t *buf);
#if (NGX_HAVE_AIO_SENDFILE)
static ngx_int_t ngx_output_chain_aio_setup(ngx_output_chain_ctx_t *ctx,
    ngx_file_t *file);
#endif
static ngx_int_t ngx_output_chain_add_copy(ngx_pool_t *pool,
    ngx_chain_t **chain, ngx_chain_t *in);
static ngx_int_t ngx_output_chain_align_file_buf(ngx_output_chain_ctx_t *ctx,
    off_t bsize);
static ngx_int_t ngx_output_chain_get_buf(ngx_output_chain_ctx_t *ctx,
    off_t bsize);
static ngx_int_t ngx_output_chain_copy_buf(ngx_output_chain_ctx_t *ctx);

/*
����Ŀ���Ƿ��� in �е����ݣ�ctx �������淢�͵������ģ���Ϊ����ͨ������£�����һ����ɡ�nginx ��Ϊʹ���� ET ģʽ��
���������¼������ϼ��ˣ����Ǳ���д����¼������ˣ���Ҫ��ͣ��ѭ�����������¼��ĺ����ص�������Ҳ��ȷ���������
Ҫʹ�� context �����Ķ��������淢�͵�ʲô�����ˡ�
*/
//���ngx_http_xxx_create_request(ngx_http_fastcgi_create_request)�Ķ���ctx->in�е�����ʵ�����Ǵ�ngx_http_xxx_create_request���ngx_chain_t���ģ�������Դ��ngx_http_xxx_create_request
ngx_int_t //���˷�������ĵ��ù���ngx_http_upstream_send_request_body->ngx_output_chain->ngx_chain_writer
ngx_output_chain(ngx_output_chain_ctx_t *ctx, ngx_chain_t *in)
{//ctxΪ&u->output�� inΪu->request_bufs����nginx filter����Ҫ�߼����������������,��in���������Ļ���鿽����
//ctx->in,Ȼ��ctx->in�����ݿ�����out,Ȼ�����output_filter���ͳ�ȥ��
    off_t         bsize;
    ngx_int_t     rc, last;
    ngx_chain_t  *cl, *out, **last_out;

    if (ctx->in == NULL && ctx->busy == NULL
#if (NGX_HAVE_FILE_AIO || NGX_THREADS)
        && !ctx->aio
#endif
       ) //in�Ǵ����͵����ݣ�busy���Ѿ�����ngx_chain_writer����û�з�����ϡ�
    {
        /*
         * the short path for the case when the ctx->in and ctx->busy chains
         * are empty, the incoming chain is empty too or has the single buf
         * that does not require the copy
         */

        if (in == NULL) { //���Ҫ���͵�����Ϊ�գ�Ҳ����ɶҲ���÷��͡��Ǿ�ֱ�ӵ���output_filter���ˡ�
            return ctx->output_filter(ctx->filter_ctx, in);
        }

        if (in->next == NULL //˵������bufֻ��һ��
#if (NGX_SENDFILE_LIMIT)
            && !(in->buf->in_file && in->buf->file_last > NGX_SENDFILE_LIMIT)
#endif
            && ngx_output_chain_as_is(ctx, in->buf)) //���������Ҫ�����ж��Ƿ���Ҫ����buf������1,��ʾ����Ҫ����������Ϊ��Ҫ���� 
        {
            return ctx->output_filter(ctx->filter_ctx, in);
        }
    }

    /* add the incoming buf to the chain ctx->in */

    if (in) {//����һ�����ݵ�ctx->in���棬��Ҫ����ʵʵ�Ľ������ݿ����ˡ���in������������ݿ�����ctx->in���档���˸�in
        if (ngx_output_chain_add_copy(ctx->pool, &ctx->in, in) == NGX_ERROR) {
            return NGX_ERROR;
        }
    }

    /* outΪ������Ҫ�����chain��Ҳ���ǽ���ʣ�µ�filter������chain */  
    out = NULL;
    last_out = &out;
    last = NGX_NONE;
	//�������ˣ�in�����Ļ��������Ѿ�������ctx->in�����ˡ�����׼�����Ͱɡ�

    for ( ;; ) {

#if (NGX_HAVE_FILE_AIO || NGX_THREADS)
        if (ctx->aio) {
            return NGX_AGAIN;
        }
#endif
        //���ngx_http_xxx_create_request(ngx_http_fastcgi_create_request)�Ķ���ctx->in�е�����ʵ�����Ǵ�ngx_http_xxx_create_request���ngx_chain_t���ģ�������Դ��ngx_http_xxx_create_request
        while (ctx->in) {//�������д����͵����ݡ�������һ����������outָ���������

            /*
             * cycle while there are the ctx->in bufs
             * and there are the free output bufs to copy in
             */

            bsize = ngx_buf_size(ctx->in->buf);
            //����ڴ��СΪ0��Ȼ���ֲ���special ���������⡣ 
            if (bsize == 0 && !ngx_buf_special(ctx->in->buf)) {

                ngx_log_error(NGX_LOG_ALERT, ctx->pool->log, 0,
                              "zero size buf in output "
                              "t:%d r:%d f:%d %p %p-%p %p %O-%O",
                              ctx->in->buf->temporary,
                              ctx->in->buf->recycled,
                              ctx->in->buf->in_file,
                              ctx->in->buf->start,
                              ctx->in->buf->pos,
                              ctx->in->buf->last,
                              ctx->in->buf->file,
                              ctx->in->buf->file_pos,
                              ctx->in->buf->file_last);

                ngx_debug_point();

                ctx->in = ctx->in->next;

                continue;
            }
            /* �ж��Ƿ���Ҫ����buf */    
            if (ngx_output_chain_as_is(ctx, ctx->in->buf)) {
                //��ctx->in->buf��ctx->in����ȡ������Ȼ����뵽lst_out������
                /* move the chain link to the output chain */
                /* �������Ҫ���ƣ���ֱ������chain��out��Ȼ�����ѭ�� */ 
                cl = ctx->in;
                ctx->in = cl->next;

                *last_out = cl;
                last_out = &cl->next;
                cl->next = NULL;

                continue;
            }
            
            /* �������˵��������Ҫ����buf������buf���ն��ᱻ������ctx->buf�У� ����������ж�ctx->buf�Ƿ�Ϊ�� */ 
            if (ctx->buf == NULL) { //ÿ�ο�������ǰ���ȸ�ctx->buf����ռ䣬�������ngx_output_chain_get_buf������

                /* ���Ϊ�գ���ȡ��buf������Ҫע�⣬һ����˵���û�п���directio�Ļ�������������᷵��NGX_DECLINED */  
                rc = ngx_output_chain_align_file_buf(ctx, bsize);

                if (rc == NGX_ERROR) {
                    return NGX_ERROR;
                }

                if (rc != NGX_OK) {

                    if (ctx->free) {

                        /* get the free buf */

                        cl = ctx->free;
                        /* �õ�free buf */    
                        ctx->buf = cl->buf;
                        ctx->free = cl->next;
                        /* ��Ҫ���õ�chain���ӵ�ctx->poll�У��Ա���chain������ */  
                        ngx_free_chain(ctx->pool, cl);

                    } else if (out || ctx->allocated == ctx->bufs.num) {
                   /* 
                        ����Ѿ�����buf�ĸ������ƣ�������ѭ���������Ѿ����ڵ�buf�� ������Կ������out���ڵĻ���nginx������ѭ����Ȼ����out��
                        �ȷ�������ٴδ���������ܺõ�������nginx����ʽ���� 
                        */ 
                        break;

                    } else if (ngx_output_chain_get_buf(ctx, bsize) != NGX_OK) {/* �����������Ҳ�ȽϹؼ���������ȡ��buf������������ϸ��������� */
                        return NGX_ERROR;
                    }
                }
            }
            
            /* ��ԭ����buf�п������ݻ��ߴ�ԭ�����ļ��ж�ȡ���� */ 
            rc = ngx_output_chain_copy_buf(ctx);

            if (rc == NGX_ERROR) {
                return rc;
            }

            if (rc == NGX_AGAIN) {
                if (out) {
                    break;
                }

                return rc;
            }

            /* delete the completed buf from the ctx->in chain */

            if (ngx_buf_size(ctx->in->buf) == 0) {//����ڵ��СΪ0���ƶ�����һ���ڵ㡣
                ctx->in = ctx->in->next;
            }

            cl = ngx_alloc_chain_link(ctx->pool);
            if (cl == NULL) {
                return NGX_ERROR;
            }
            //��ngx_output_chain_copy_buf�д�ԭsrc���������ݸ�ֵ��cl->buf��Ȼ�����ӵ�lst_out��ͷ��
            cl->buf = ctx->buf;
            cl->next = NULL;
            *last_out = cl;
            last_out = &cl->next;
            ctx->buf = NULL;
        }

        if (out == NULL && last != NGX_NONE) {

            if (ctx->in) {
                return NGX_AGAIN;
            }

            return last;
        }

        last = ctx->output_filter(ctx->filter_ctx, out); //ngx_chain_writer

        if (last == NGX_ERROR || last == NGX_DONE) {
            return last;
        }

        ngx_chain_update_chains(ctx->pool, &ctx->free, &ctx->busy, &out,
                                ctx->tag);
        last_out = &out;
    }
}


static ngx_inline ngx_int_t
ngx_output_chain_as_is(ngx_output_chain_ctx_t *ctx, ngx_buf_t *buf)
{//��������ڵ��Ƿ���Կ��������content�Ƿ����ļ��С��ж��Ƿ���Ҫ����buf.
//����1��ʾ�ϲ㲻��Ҫ����buf,������Ҫ����allocһ���ڵ㣬����ʵ���ڴ浽����һ���ڵ㡣
    ngx_uint_t  sendfile;

    if (ngx_buf_special(buf)) { //˵��buf��û��ʵ������
        return 1;
    }

#if (NGX_THREADS)
    if (buf->in_file) {
        buf->file->thread_handler = ctx->thread_handler;
        buf->file->thread_ctx = ctx->filter_ctx;
    }
#endif

    if (buf->in_file && buf->file->directio) {
        return 0;//���buf���ļ��У�ʹ����directio����Ҫ����buf
    }

    sendfile = ctx->sendfile;

#if (NGX_SENDFILE_LIMIT)

    if (buf->in_file && buf->file_pos >= NGX_SENDFILE_LIMIT) {
        sendfile = 0;
    }

#endif

    if (!sendfile) {

        if (!ngx_buf_in_memory(buf)) {
            return 0;
        }

        buf->in_file = 0;
    }

#if (NGX_HAVE_AIO_SENDFILE)
    if (ctx->aio_preload && buf->in_file) {
        (void) ngx_output_chain_aio_setup(ctx, buf->file);
    }
#endif
    /* (ʹ��sendfile�Ļ����ڴ���û���ļ��Ŀ����ģ���������ʱ��Ҫ�����ļ��������Ҫ�����ļ�����*/
    if (ctx->need_in_memory && !ngx_buf_in_memory(buf)) {
        return 0;
    }

    if (ctx->need_in_temp && (buf->memory || buf->mmap)) {
        return 0;
    }

    return 1;
}


#if (NGX_HAVE_AIO_SENDFILE)

static ngx_int_t
ngx_output_chain_aio_setup(ngx_output_chain_ctx_t *ctx, ngx_file_t *file)
{
    ngx_event_aio_t  *aio;

    if (file->aio == NULL && ngx_file_aio_init(file, ctx->pool) != NGX_OK) {
        return NGX_ERROR;
    }

    aio = file->aio;

    aio->data = ctx->filter_ctx;
    aio->preload_handler = ctx->aio_preload;

    return NGX_OK;
}

#endif

//���»�ȡһ��ngx_chain_t�ṹ���ýṹ��bufָ��in->buf���ú������µ�ngx_chain_t���ӵ�chain����ĩβ
static ngx_int_t
ngx_output_chain_add_copy(ngx_pool_t *pool, ngx_chain_t **chain,
    ngx_chain_t *in)
{//ngx_output_chain���������u->request_bufsҲ���ǲ��� in�����ݿ�����chain���档
//����Ϊ:(ctx->pool, &ctx->in, in)��in����Ҫ���͵ģ�Ҳ��������Ļ�����������
    ngx_chain_t  *cl, **ll;
#if (NGX_SENDFILE_LIMIT)
    ngx_buf_t    *b, *buf;
#endif

    ll = chain;

    for (cl = *chain; cl; cl = cl->next) {
        ll = &cl->next; //llָ��chain��ĩβ
    }

    while (in) {

        cl = ngx_alloc_chain_link(pool);
        if (cl == NULL) {
            return NGX_ERROR;
        }

#if (NGX_SENDFILE_LIMIT)

        buf = in->buf;

        if (buf->in_file
            && buf->file_pos < NGX_SENDFILE_LIMIT
            && buf->file_last > NGX_SENDFILE_LIMIT)
        {//�������buffer���ļ��У������ļ�û�г������ƣ��ǾͿ����������ǣ��������ļ�������limit������ô�죬��ֳ�2��buffer��
            /* split a file buf on two bufs by the sendfile limit */

            b = ngx_calloc_buf(pool);
            if (b == NULL) {
                return NGX_ERROR;
            }

            ngx_memcpy(b, buf, sizeof(ngx_buf_t));

            if (ngx_buf_in_memory(buf)) {
                buf->pos += (ssize_t) (NGX_SENDFILE_LIMIT - buf->file_pos);
                b->last = buf->pos;
            }

            buf->file_pos = NGX_SENDFILE_LIMIT;
            b->file_last = NGX_SENDFILE_LIMIT;

            cl->buf = b;

        } else {
            cl->buf = buf;
            in = in->next;
        }

#else
        cl->buf = in->buf;
        in = in->next;

#endif

        cl->next = NULL;
        *ll = cl;
        ll = &cl->next;
    }

    return NGX_OK;
}


static ngx_int_t
ngx_output_chain_align_file_buf(ngx_output_chain_ctx_t *ctx, off_t bsize)
{
    size_t      size;
    ngx_buf_t  *in;

    in = ctx->in->buf;

    if (in->file == NULL || !in->file->directio) {
        return NGX_DECLINED;
    }

    ctx->directio = 1;

    size = (size_t) (in->file_pos - (in->file_pos & ~(ctx->alignment - 1)));

    if (size == 0) {

        if (bsize >= (off_t) ctx->bufs.size) {
            return NGX_DECLINED;
        }

        size = (size_t) bsize;

    } else {
        size = (size_t) ctx->alignment - size;

        if ((off_t) size > bsize) {
            size = (size_t) bsize;
        }
    }

    ctx->buf = ngx_create_temp_buf(ctx->pool, size);
    if (ctx->buf == NULL) {
        return NGX_ERROR;
    }

    /*
     * we do not set ctx->buf->tag, because we do not want
     * to reuse the buf via ctx->free list
     */

#if (NGX_HAVE_ALIGNED_DIRECTIO)
    ctx->unaligned = 1;
#endif

    return NGX_OK;
}


static ngx_int_t
ngx_output_chain_get_buf(ngx_output_chain_ctx_t *ctx, off_t bsize)
{
    size_t       size;
    ngx_buf_t   *b, *in;
    ngx_uint_t   recycled;

    in = ctx->in->buf;
    size = ctx->bufs.size;
    recycled = 1;

    if (in->last_in_chain) {

        if (bsize < (off_t) size) {

            /*
             * allocate a small temp buf for a small last buf
             * or its small last part
             */

            size = (size_t) bsize;
            recycled = 0;

        } else if (!ctx->directio
                   && ctx->bufs.num == 1
                   && (bsize < (off_t) (size + size / 4)))
        {
            /*
             * allocate a temp buf that equals to a last buf,
             * if there is no directio, the last buf size is lesser
             * than 1.25 of bufs.size and the temp buf is single
             */

            size = (size_t) bsize;
            recycled = 0;
        }
    }

    b = ngx_calloc_buf(ctx->pool);
    if (b == NULL) {
        return NGX_ERROR;
    }

    if (ctx->directio) {

        /*
         * allocate block aligned to a disk sector size to enable
         * userland buffer direct usage conjunctly with directio
         */

        b->start = ngx_pmemalign(ctx->pool, size, (size_t) ctx->alignment);
        if (b->start == NULL) {
            return NGX_ERROR;
        }

    } else {
        b->start = ngx_palloc(ctx->pool, size);
        if (b->start == NULL) {
            return NGX_ERROR;
        }
    }

    b->pos = b->start;
    b->last = b->start;
    b->end = b->last + size;
    b->temporary = 1;
    b->tag = ctx->tag;
    b->recycled = recycled;

    ctx->buf = b;
    ctx->allocated++;

    return NGX_OK;
}


static ngx_int_t
ngx_output_chain_copy_buf(ngx_output_chain_ctx_t *ctx)
{//��ctx->in->buf�Ļ��忽����ctx->buf����ȥ��  ע���Ǵ��·��������ݿռ䣬�����洢ԭ����in->buf�е����ݣ�ʵ�������ھ���������ͬ��������
    off_t        size;
    ssize_t      n;
    ngx_buf_t   *src, *dst;
    ngx_uint_t   sendfile;

    src = ctx->in->buf;//���ngx_http_xxx_create_request(ngx_http_fastcgi_create_request)�Ķ���ctx->in�е�����ʵ�����Ǵ�ngx_http_xxx_create_request���ngx_chain_t���ģ�������Դ��ngx_http_xxx_create_request
    dst = ctx->buf;

    size = ngx_buf_size(src);
    size = ngx_min(size, dst->end - dst->pos);

    sendfile = ctx->sendfile & !ctx->directio;

#if (NGX_SENDFILE_LIMIT)

    if (src->in_file && src->file_pos >= NGX_SENDFILE_LIMIT) {
        sendfile = 0;
    }

#endif

    if (ngx_buf_in_memory(src)) {//����������ڴ��ֱ�ӽ��п�������
        ngx_memcpy(dst->pos, src->pos, (size_t) size); 
        //�����sizeΪʲô�ܱ�֤��Խ�磬����Ϊ�����ڴ��ʱ������ngx_output_chain_get_buf��ʱ��bsize�͵���bsize = ngx_buf_size(ctx->in->buf);
        src->pos += (size_t) size;
        dst->last += (size_t) size; //ע��dst->pose��û���ƶ�

        if (src->in_file) {

            if (sendfile) {
                dst->in_file = 1;
                dst->file = src->file;
                dst->file_pos = src->file_pos;
                dst->file_last = src->file_pos + size;

            } else {
                dst->in_file = 0;
            }

            src->file_pos += size;

        } else {
            dst->in_file = 0;
        }

        if (src->pos == src->last) {  
            dst->flush = src->flush;
            dst->last_buf = src->last_buf;
            dst->last_in_chain = src->last_in_chain;
        }

    } else {//�����ļ������ڴ����棬��Ҫ�Ӵ��̶�ȡ��

#if (NGX_HAVE_ALIGNED_DIRECTIO)

        if (ctx->unaligned) {
            if (ngx_directio_off(src->file->fd) == NGX_FILE_ERROR) {
                ngx_log_error(NGX_LOG_ALERT, ctx->pool->log, ngx_errno,
                              ngx_directio_off_n " \"%s\" failed",
                              src->file->name.data);
            }
        }

#endif

#if (NGX_HAVE_FILE_AIO)
        if (ctx->aio_handler) {
            n = ngx_file_aio_read(src->file, dst->pos, (size_t) size,
                                  src->file_pos, ctx->pool);
            if (n == NGX_AGAIN) {
                ctx->aio_handler(ctx, src->file);
                return NGX_AGAIN;
            }

        } else
#endif
#if (NGX_THREADS)
        if (src->file->thread_handler) {
            n = ngx_thread_read(&ctx->thread_task, src->file, dst->pos,
                                (size_t) size, src->file_pos, ctx->pool);
            if (n == NGX_AGAIN) {
                ctx->aio = 1;
                return NGX_AGAIN;
            }

        } else
#endif
        {
            n = ngx_read_file(src->file, dst->pos, (size_t) size,
                              src->file_pos); //��src->file�ļ���src->file_pos����ȡsize�ֽڵ�dst->posָ����ڴ�ռ�
        }

#if (NGX_HAVE_ALIGNED_DIRECTIO)

        if (ctx->unaligned) {
            ngx_err_t  err;

            err = ngx_errno;

            if (ngx_directio_on(src->file->fd) == NGX_FILE_ERROR) {
                ngx_log_error(NGX_LOG_ALERT, ctx->pool->log, ngx_errno,
                              ngx_directio_on_n " \"%s\" failed",
                              src->file->name.data);
            }

            ngx_set_errno(err);

            ctx->unaligned = 0;
        }

#endif

        if (n == NGX_ERROR) {
            return (ngx_int_t) n;
        }

        if (n != size) {
            ngx_log_error(NGX_LOG_ALERT, ctx->pool->log, 0,
                          ngx_read_file_n " read only %z of %O from \"%s\"",
                          n, size, src->file->name.data);
            return NGX_ERROR;
        }

        dst->last += n; //pos��lstָ����ƶ�n�ֽڣ���ʾ�ڴ��ж�����ô�࣬ע��posû���ƶ�

        if (sendfile) { //�����sendfile��ͨ�������ngx_read_file��Ӵ����ļ���ȡһ�ݵ��û��ռ�
            dst->in_file = 1; //��ʶ��buf����in_file
            dst->file = src->file;
            dst->file_pos = src->file_pos;
            dst->file_last = src->file_pos + n;

        } else {
            dst->in_file = 0; //����sendfile�ģ�ֱ�Ӱ�in_file��0
        }

        src->file_pos += n; //file_pos�����ƶ�n�ֽڣ���ʾ��n�ֽ��Ѿ���ȡ���ڴ���

        if (src->file_pos == src->file_last) { //�����е������Ѿ�ȫ����ȡ��Ӧ�ò��ڴ���  
            dst->flush = src->flush;
            dst->last_buf = src->last_buf;
            dst->last_in_chain = src->last_in_chain;
        }
    }

    return NGX_OK;
}

//���˷�������ĵ��ù���ngx_http_upstream_send_request_body->ngx_output_chain->ngx_chain_writer
ngx_int_t
ngx_chain_writer(void *data, ngx_chain_t *in)
{
    ngx_chain_writer_ctx_t *ctx = data;

    off_t              size;
    ngx_chain_t       *cl, *ln, *chain;
    ngx_connection_t  *c;

    c = ctx->connection;
    /*�����ѭ������in�����ÿһ�����ӽڵ㣬���ӵ�ctx->filter_ctx��ָ�������С�����¼��Щin�������Ĵ�С��*/
    for (size = 0; in; in = in->next) {

#if 1
        if (ngx_buf_size(in->buf) == 0 && !ngx_buf_special(in->buf)) {

            ngx_log_error(NGX_LOG_ALERT, ctx->pool->log, 0,
                          "zero size buf in chain writer "
                          "t:%d r:%d f:%d %p %p-%p %p %O-%O",
                          in->buf->temporary,
                          in->buf->recycled,
                          in->buf->in_file,
                          in->buf->start,
                          in->buf->pos,
                          in->buf->last,
                          in->buf->file,
                          in->buf->file_pos,
                          in->buf->file_last);

            ngx_debug_point();

            continue;
        }
#endif

        size += ngx_buf_size(in->buf);

        ngx_log_debug2(NGX_LOG_DEBUG_CORE, c->log, 0,
                       "chain writer buf fl:%d s:%uO",
                       in->buf->flush, ngx_buf_size(in->buf));

        cl = ngx_alloc_chain_link(ctx->pool);
        if (cl == NULL) {
            return NGX_ERROR;
        }

        cl->buf = in->buf; //��in->buf��ֵ���µ�cl->buf��
        cl->next = NULL;
        //����������ʵ���Ͼ��ǰ�cl���ӵ�ctx->out����ͷ�У�
        *ctx->last = cl; 
        ctx->last = &cl->next; //����ƶ�lastָ�룬ָ���µ����һ���ڵ��next������ַ���ٴ�ѭ���ߵ������ʱ�򣬵���ctx->last=cl����µ�cl���ӵ�out��β��
    }

    ngx_log_debug1(NGX_LOG_DEBUG_CORE, c->log, 0,
                   "chain writer in: %p", ctx->out);
                   
    //�����ո�׼������������ͳ�����С������ɶ��˼?ctx->outΪ����ͷ��������������������еġ�
    for (cl = ctx->out; cl; cl = cl->next) {

#if 1
        if (ngx_buf_size(cl->buf) == 0 && !ngx_buf_special(cl->buf)) {

            ngx_log_error(NGX_LOG_ALERT, ctx->pool->log, 0,
                          "zero size buf in chain writer "
                          "t:%d r:%d f:%d %p %p-%p %p %O-%O",
                          cl->buf->temporary,
                          cl->buf->recycled,
                          cl->buf->in_file,
                          cl->buf->start,
                          cl->buf->pos,
                          cl->buf->last,
                          cl->buf->file,
                          cl->buf->file_pos,
                          cl->buf->file_last);

            ngx_debug_point();

            continue;
        }
#endif

        size += ngx_buf_size(cl->buf);
    }

    if (size == 0 && !c->buffered) {//ɶ���ݶ�ô�У����÷��˶�
        return NGX_OK;
    }

    //����writev��ctx->out������ȫ�����ͳ�ȥ�����û�����꣬�򷵻�û������ϵĲ��֡���¼��out����
	//��ngx_event_connect_peer�������η�������ʱ�����õķ������Ӻ���ngx_send_chain=ngx_writev_chain��
    chain = c->send_chain(c, ctx->out, ctx->limit); //ngx_send_chain->ngx_writev_chain  ����˵��������ǲ�����filter����ģ��ģ�����ֱ�ӵ���ngx_writev_chain->ngx_writev���͵����

    ngx_log_debug1(NGX_LOG_DEBUG_CORE, c->log, 0,
                   "chain writer out: %p", chain);

    if (chain == NGX_CHAIN_ERROR) {
        return NGX_ERROR;
    }

    for (cl = ctx->out; cl && cl != chain; /* void */) { //��ctx->out���Ѿ�ȫ�����ͳ�ȥ��in�ڵ��out����ժ������free�У��ظ�����
        ln = cl;
        cl = cl->next;
        ngx_free_chain(ctx->pool, ln);
    }

    ctx->out = chain; //ctx->out��������ֻʣ�»�û�з��ͳ�ȥ��in�ڵ���

    if (ctx->out == NULL) { //˵���Ѿ�ctx->out���е����������Ѿ�ȫ���������
        ctx->last = &ctx->out;

        if (!c->buffered) { 
        //���͵���˵�������֮ǰbufferedһֱ��û�в�����Ϊ0�������Ӧ����ͻ��˵���Ӧ����buffered�����ڽ���ngx_http_write_filter����
        //c->send_chain()֮ǰ�Ѿ��и�ֵ�������͸��ͻ��˰����ʱ��ᾭ�����е�filterģ���ߵ�����
            return NGX_OK;
        }
    }

    return NGX_AGAIN;
}