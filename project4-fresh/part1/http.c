#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include "http.h"

#define BUFSIZE 512
#define CHUNKSIZE (8*BUFSIZE)


typedef struct content_info {
    char mime_type[BUFSIZE];
    size_t length;
} content_info_t;


const char *get_mime_type(const char *file_extension) {
    if (strcmp(".txt", file_extension) == 0) {
        return "text/plain";
    } else if (strcmp(".html", file_extension) == 0) {
        return "text/html";
    } else if (strcmp(".jpg", file_extension) == 0) {
        return "image/jpeg";
    } else if (strcmp(".png", file_extension) == 0) {
        return "image/png";
    } else if (strcmp(".pdf", file_extension) == 0) {
        return "application/pdf";
    }

    return NULL;
}


int extract_content_info(const char *resource_path,
        content_info_t *content_info) {
    memset(content_info, 0, sizeof(content_info_t));

    // get file extension
    const char *file_extension;
    if ((file_extension = strrchr(resource_path, '.')) == NULL) {
        fprintf(stderr, "Failed to retreive file extension\n");
        return -1;
    }

    // get mime type
    const char *mime_type = get_mime_type(file_extension);
    if (mime_type == NULL) {
        fprintf(stderr, "Failed to get mime type\n");
        return -1;
    }

    // get content length
    struct stat file_stat;
    if (stat(resource_path, &file_stat) != 0) { perror("stat"); return -1; }

    // populate content_info fields
    content_info->length = file_stat.st_size;
    strncpy(content_info->mime_type, mime_type, strlen(mime_type));

    return 0;
}


int read_http_request(int fd, char *resource_name) {
    char buf[BUFSIZE];
    memset(buf, 0, BUFSIZE);

    int bytes_read;

    // discard first four characters ("GET ")
    bytes_read = read(fd, buf, 4*sizeof(char));
    if (bytes_read == 4*sizeof(char)) {}
    else if (bytes_read == -1) 
        { perror("read"); return -1; } 
    else {
        // http request is less than four characters long
        fprintf(stderr, "Bad HTTP request\n"); return -1;
    }

    // retrieve resource name -- assuming it's under BUFSIZE characters (throw error otherwise)
    bytes_read = read(fd, buf, BUFSIZE);
    if (bytes_read == -1) 
        { perror("read"); return -1; } 
    char *saveptr = NULL;  // for strtok_r
    char *name = strtok_r(buf, " ", &saveptr);
    if (name == NULL) {
        fprintf(stderr, "Resource name is too long or HTTP request is badly formatted\n");
        return -1;
    }

    strcpy(resource_name, name);

    return 0;
}


int write_http_response(int fd, const char *resource_path) {
    int status_val = 200;
    int bytes_read = 0;
    int bytes_written = 0;
    FILE *file;

    // make sure file can be opened -- different headers depending on
    // success/failure failure to open need not yield a -1 return error value
    // -- we indicate error in the HTTP response
    if (strcmp("", resource_path) == 0) {
        status_val = 404;
    } else {
        file = fopen(resource_path, "r");
        if (file == NULL) { status_val = 404; }
    }

    // Pretend as if directory files do not exist, since we do not provide
    // a facility for listing their contents like real HTTP servers do
    struct stat statbuf;
    // Note that if stat errors, we just assume the file is not usable
    // and send a 404 rather than crashing
    if (file && (stat(resource_path, &statbuf) != 0 || S_ISDIR(statbuf.st_mode))) {
        status_val = 404;
        if (fclose(file) == -1) { perror("close"); return -1; }
    }

    // write HTTP response header -- return on 404 status
    if (status_val == 404) {
        // construct header
        char notfound[] = "HTTP/1.0 404 Not Found\r\nContent-Length: 0\r\n\r\n";
        size_t nfsize = strlen(notfound);
        // Write 404 header
        if ((bytes_written = write(fd, notfound, nfsize)) != nfsize) {
            if (bytes_written < 0) { perror("write"); }
            else { fprintf(stderr, "Failed to write HTTP response header\n"); }
            return -1;
        }
        // clean up and return
        return 0;
    }

    // extract content type and length
    content_info_t content_info;
    if (extract_content_info(resource_path, &content_info) == -1) {
        fprintf(stderr, "Failed to extract content info\n");
        fclose(file);
        return -1;
    }
    // FIXED
    // Declare chunk to write
    char chunk[CHUNKSIZE];
    unsigned int offset = 0;

    // Add header to chunk
    int res = sprintf(chunk,
            "HTTP/1.0 200 OK\r\nContent-Type: %s\r\nContent-Length: %ld\r\n\r\n",
            content_info.mime_type,
            content_info.length);
    if (res < 0) {
        perror("sprintf");
        if (fclose(file) == EOF) {
            perror("sprintf");
        }
        return -1;
    }
    offset = strlen(chunk);

    // Write file chunk by chunk
    do {
        // Read into the chunk
        bytes_read = fread(chunk + offset, 1, CHUNKSIZE - offset, file);
        if (ferror(file)) {
            perror("fread");
            if (fclose(file) == EOF) { perror("fclose"); }
            return -1;
        }

        // Write the chunk
        bytes_written = write(fd, chunk, bytes_read + offset) == -1;
        if (bytes_written == -1) {
            perror("fread");
            if (fclose(file) == EOF) { perror("fclose"); }
            return -1;
        }

        // Reset the offset after the first write - we only need to write the
        // header once.
        offset = 0;
    } while (bytes_read > 0);

    return 0;
}
