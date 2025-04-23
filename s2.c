#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <dirent.h>

#define PORT 7082
#define BUFFER_SIZE 1024
#define MAX_PATH 512
#define TAR_BUFFER_SIZE (10 * 1024 * 1024)  // 10MB buffer for tar files

void expand_path(const char *input_path, char *output_path, size_t size) {
    if (input_path[0] == '~') {
        const char *home = getenv("HOME");
        if (!home) {
            fprintf(stderr, "Error: HOME environment variable not set.\n");
            exit(1);
        }
        
        if (strncmp(input_path, "~s2", 3) == 0) {
            snprintf(output_path, size, "%s/s2%s", home, input_path + 3);
        } else {
            snprintf(output_path, size, "%s%s", home, input_path + 1);
        }
    } else {
        strncpy(output_path, input_path, size);
    }
}

void create_directory(const char *path) {
    char cmd[MAX_PATH + 50];
    snprintf(cmd, sizeof(cmd), "mkdir -p %s", path);
    system(cmd);
}

int handle_download_request(int client_fd, const char *filepath) {
    char expanded_path[MAX_PATH];
    expand_path(filepath, expanded_path, sizeof(expanded_path));

    FILE *fp = fopen(expanded_path, "rb");
    if (!fp) {
        printf("[S2] Requested PDF not found: %s\n", expanded_path);
        return -1;
    }

    printf("[S2] Sending PDF file: %s\n", expanded_path);
    char buffer[BUFFER_SIZE];
    int bytes_read;
    while ((bytes_read = fread(buffer, 1, sizeof(buffer), fp)) > 0) {
        if (send(client_fd, buffer, bytes_read, 0) == -1) {
            perror("[S2] Failed to send file data");
            fclose(fp);
            return -1;
        }
    }
    fclose(fp);
    return 0;
}

int handle_upload_request(int client_fd) {
    char dest_path[MAX_PATH], filename[256], buffer[BUFFER_SIZE];

    // Receive destination path
    memset(dest_path, 0, sizeof(dest_path));
    int len = recv(client_fd, dest_path, sizeof(dest_path), 0);
    if (len <= 0) {
        perror("[S2] Failed to receive destination path");
        return -1;
    }
    dest_path[len] = '\0';

    // Receive filename
    memset(filename, 0, sizeof(filename));
    len = recv(client_fd, filename, sizeof(filename), 0);
    if (len <= 0) {
        perror("[S2] Failed to receive filename");
        return -1;
    }
    filename[len] = '\0';

    // Expand path (convert ~s2 to /home/user/s2)
    char expanded_path[MAX_PATH];
    expand_path(dest_path, expanded_path, sizeof(expanded_path));

    // Create directory structure
    char dir_path[MAX_PATH];
    strncpy(dir_path, expanded_path, MAX_PATH);
    char *last_slash = strrchr(dir_path, '/');
    if (last_slash) {
        *last_slash = '\0';
        create_directory(dir_path);
    }

    // Create full path
    char filepath[MAX_PATH];
    snprintf(filepath, sizeof(filepath), "%s/%s", expanded_path, filename);

    FILE *fp = fopen(filepath, "wb");
    if (!fp) {
        perror("[S2] File open failed");
        return -1;
    }

    // Receive file content
    int bytes_received;
    while ((bytes_received = recv(client_fd, buffer, sizeof(buffer), 0)) > 0) {
        fwrite(buffer, 1, bytes_received, fp);
        if (bytes_received < sizeof(buffer)) break;
    }

    fclose(fp);
    printf("[S2] Received and saved PDF: %s\n", filepath);
    return 0;
}


int create_pdf_tar(const char *output_path) {
    char cmd[2 * MAX_PATH];
    snprintf(cmd, sizeof(cmd), "find ~/s2 -type f -name \"*.pdf\" -print0 | tar -cf %s --null -T - 2>/dev/null", 
             output_path);
    
    int ret = system(cmd);
    if (ret != 0) {
        fprintf(stderr, "[S2] Failed to create PDF tar archive at %s\n", output_path);
        return -1;
    }
    return 0;
}



int handle_tar_request(int client_fd, const char *requested_name) {
    // Create temporary tar file with the requested name
    char temp_dir[] = "/tmp/s2_tar_XXXXXX";
    if (mkdtemp(temp_dir) == NULL) {
        perror("[S2] Failed to create temp directory");
        return -1;
    }

    char tar_path[MAX_PATH];
    snprintf(tar_path, sizeof(tar_path), "%s/%s", temp_dir, requested_name);

    // Create the PDF tar archive
    if (create_pdf_tar(tar_path) != 0) {
        // Clean up
        char cmd[MAX_PATH + 50];
        snprintf(cmd, sizeof(cmd), "rm -rf %s", temp_dir);
        system(cmd);
        return -1;
    }

    // Open and send the tar file
    FILE *fp = fopen(tar_path, "rb");
    if (!fp) {
        perror("[S2] Failed to open tar file");
        // Clean up
        char cmd[MAX_PATH + 50];
        snprintf(cmd, sizeof(cmd), "rm -rf %s", temp_dir);
        system(cmd);
        return -1;
    }

    // Send file content directly
    char buffer[BUFFER_SIZE];
    size_t bytes_read;
    while ((bytes_read = fread(buffer, 1, sizeof(buffer), fp)) > 0) {
        if (send(client_fd, buffer, bytes_read, 0) == -1) {
            perror("[S2] Error sending tar file");
            break;
        }
    }

    fclose(fp);
    
    // Clean up
    char cmd[MAX_PATH + 50];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", temp_dir);
    system(cmd);
    
    return 0;
}

int main() {
    int server_fd, client_fd;
    struct sockaddr_in server_addr, client_addr;
    socklen_t addr_len = sizeof(client_addr);

    // Create socket
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1) {
        perror("[S2] Socket creation failed");
        exit(1);
    }

    // Reuse port
    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("[S2] setsockopt failed");
        exit(1);
    }

    // Configure server address
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    // Bind
    if (bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) == -1) {
        perror("[S2] Bind failed");
        exit(1);
    }

    // Listen
    if (listen(server_fd, 5) == -1) {
        perror("[S2] Listen failed");
        exit(1);
    }

    printf("[S2] Server is listening on port %d...\n", PORT);

    while (1) {
        client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &addr_len);
        if (client_fd == -1) {
            perror("[S2] Accept failed");
            continue;
        }

        // Determine request type (upload or download)
        char request_type[20];
        int len = recv(client_fd, request_type, sizeof(request_type) - 1, MSG_PEEK);
        if (len <= 0) {
            perror("[S2] Failed to determine request type");
            close(client_fd);
            continue;
        }
        request_type[len] = '\0';

        if (strncmp(request_type, "DOWNLOAD ", 9) == 0) {
            // Handle download request
            char request[BUFFER_SIZE];
            len = recv(client_fd, request, sizeof(request) - 1, 0);
            if (len <= 0) {
                perror("[S2] Failed to receive download request");
                close(client_fd);
                continue;
            }
            request[len] = '\0';

            char *filepath = request + 9; // Skip "DOWNLOAD " prefix
            handle_download_request(client_fd, filepath);
        } 
      
        else if (strncmp(request_type, "REMOVE ", 7) == 0) {
            // Handle remove request
            char request[BUFFER_SIZE];
            len = recv(client_fd, request, sizeof(request) - 1, 0);
            if (len <= 0) {
                perror("[S2] Failed to receive remove request");
                close(client_fd);
                continue;
            }
            request[len] = '\0';
        
            char *filepath = request + 7; // Skip "REMOVE " prefix
            char expanded_path[MAX_PATH];
            expand_path(filepath, expanded_path, sizeof(expanded_path));
        
            if (remove(expanded_path) == 0) {
                printf("[S2] Deleted file: %s\n", expanded_path);
                send(client_fd, "REMOVE_SUCCESS", 14, 0);
            } else {
                perror("[S2] Failed to delete file");
                send(client_fd, "REMOVE_FAILED", 13, 0);
            }
        }
        

        else if (strncmp(request_type, "TAR_PDF:", 8) == 0) {
            // Handle PDF tar request with specific filename
            char request[BUFFER_SIZE];
            len = recv(client_fd, request, sizeof(request) - 1, 0);
            if (len <= 0) {
                perror("[S2] Failed to receive tar request");
                close(client_fd);
                continue;
            }
            request[len] = '\0';
            
            // Extract the requested filename (format is "TAR_PDF:filename.tar")
            char *colon = strchr(request, ':');
            if (!colon) {
                printf("[S2] Invalid TAR_PDF request format\n");
                close(client_fd);
                continue;
            }
            char *requested_name = colon + 1;
            
            if (handle_tar_request(client_fd, requested_name) == 0) {
                printf("[S2] Sent PDF tar archive '%s' to client\n", requested_name);
            } else {
                printf("[S2] Failed to create/send PDF tar archive\n");
            }
        }
 
        else if (strncmp(request_type, "LIST ", 5) == 0) {
            char expanded_path[MAX_PATH];
            expand_path("~s2/", expanded_path, sizeof(expanded_path)); // Always use root
            
            // Find files with relative paths from server root
            char cmd[2 * MAX_PATH];
            snprintf(cmd, sizeof(cmd), "find %s -type f -name \"*.pdf\" -printf \"%%P\\n\" | sort", expanded_path);
            
            FILE *fp = popen(cmd, "r");
            char file_list[BUFFER_SIZE] = {0};
            char line[256];
            size_t list_size = 0;
            
            while (fgets(line, sizeof(line), fp)) {
                line[strcspn(line, "\n")] = 0;
                if (strlen(line) > 0) {
                    list_size += snprintf(file_list + list_size,
                                        sizeof(file_list) - list_size,
                                        "%s\n", line);
                }
            }
            pclose(fp);
            send(client_fd, file_list, list_size, 0);
        }

        else {
            // Handle upload request (original functionality)
            handle_upload_request(client_fd);
        }

        close(client_fd);
    }

    close(server_fd);
    return 0;
}