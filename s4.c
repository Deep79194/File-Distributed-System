#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>

#define S4_PORT 2022
#define BUFFER_SIZE 1024
#define MAX_PATH 512

void handle_client(int client_fd);
void expand_path(const char *input_path, char *output_path, size_t size);
int handle_download_request(int client_fd, const char *filepath);
int handle_upload_request(int client_fd);

int main() {
    int server_fd, client_fd;
    struct sockaddr_in server_addr, client_addr;
    socklen_t addr_len = sizeof(client_addr);

    // Create socket
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1) {
        perror("[S4] Socket creation failed");
        exit(EXIT_FAILURE);
    }

    // Allow port reuse
    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("[S4] setsockopt failed");
        exit(EXIT_FAILURE);
    }

    // Configure server address
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(S4_PORT);

    // Bind socket to port
    if (bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) == -1) {
        perror("[S4] Bind failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    // Start listening
    if (listen(server_fd, 5) == -1) {
        perror("[S4] Listening failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    printf("[S4] Server is listening on port %d for .zip files...\n", S4_PORT);

    while (1) {
        // Accept client connection (will be S1 in this case)
        client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &addr_len);
        if (client_fd == -1) {
            perror("[S4] Accept failed");
            continue;
        }

        // printf("[S4] S1 connected to S4\n");
        handle_client(client_fd);
        close(client_fd);
    }

    close(server_fd);
    return 0;
}

void expand_path(const char *input_path, char *output_path, size_t size) {
    if (input_path[0] == '~') {
        const char *home = getenv("HOME");
        if (!home) {
            fprintf(stderr, "[S4] Error: HOME environment variable not set.\n");
            exit(1);
        }
        
        if (strncmp(input_path, "~s4", 3) == 0) {
            snprintf(output_path, size, "%s/s4%s", home, input_path + 3);
        } else if (strcmp(input_path, "~") == 0) {
            strncpy(output_path, home, size);
        } else {
            char *slash = strchr(input_path, '/');
            if (slash) {
                snprintf(output_path, size, "%s%s", home, slash);
            } else {
                strncpy(output_path, home, size);
            }
        }
    } else {
        strncpy(output_path, input_path, size);
    }
}


int handle_download_request(int client_fd, const char *filepath) {
    char expanded_path[MAX_PATH];
    expand_path(filepath, expanded_path, sizeof(expanded_path));

    FILE *fp = fopen(expanded_path, "rb");
    if (!fp) {
        printf("[S4] Requested ZIP file not found: %s\n", expanded_path);
        send(client_fd, "FILE_NOT_FOUND", 14, 0);
        return -1;
    }

    printf("[S4] Sending ZIP file: %s\n", expanded_path);
    
    // Send file data directly
    char buffer[BUFFER_SIZE];
    int bytes_read, total_sent = 0;
    while ((bytes_read = fread(buffer, 1, sizeof(buffer), fp)) > 0) {
        int sent = send(client_fd, buffer, bytes_read, 0);
        if (sent == -1) {
            perror("[S4] Failed to send file data");
            fclose(fp);
            return -1;
        }
        total_sent += sent;
    }
    
    fclose(fp);
    printf("[S4] Sent %d bytes\n", total_sent);
    return 0;
}

int handle_upload_request(int client_fd) {
    char command[BUFFER_SIZE];
    char filename[256], destination[256], filepath[MAX_PATH], expanded_dest[MAX_PATH];

    // Receive command from S1 (format: "STORE_ZIP <filename> <destination>")
    int bytes_received = recv(client_fd, command, sizeof(command) - 1, 0);
    if (bytes_received <= 0) {
        perror("[S4] Failed to receive command from S1");
        return -1;
    }
    command[bytes_received] = '\0';

    // Parse command
    if (sscanf(command, "STORE_ZIP %s %s", filename, destination) != 2) {
        perror("[S4] Invalid command format from S1");
        send(client_fd, "STORE_FAILED", 12, 0);
        return -1;
    }

    // Expand destination path (convert ~s1 to ~s4)
    char modified_dest[MAX_PATH];
    strncpy(modified_dest, destination, MAX_PATH);
    char *s1_pos = strstr(modified_dest, "~s1");
    if (s1_pos) {
        memcpy(s1_pos, "~s4", 3);
    }

    expand_path(modified_dest, expanded_dest, sizeof(expanded_dest));

    // Create directory if it doesn't exist
    struct stat st;
    if (stat(expanded_dest, &st) == -1) {
        mkdir(expanded_dest, 0777);
    }

    // Construct full file path
    snprintf(filepath, sizeof(filepath), "%s/%s", expanded_dest, filename);

    printf("[S4] Receiving .zip file: %s, Destination: %s\n", filename, filepath);

    // Open file for writing
    FILE *fp = fopen(filepath, "wb");
    if (!fp) {
        perror("[S4] File open failed");
        send(client_fd, "STORE_FAILED", 12, 0);
        return -1;
    }

    // Receive file data and write to disk
    char buffer[BUFFER_SIZE];
    while ((bytes_received = recv(client_fd, buffer, sizeof(buffer), 0)) > 0) {
        fwrite(buffer, 1, bytes_received, fp);
        if (bytes_received < sizeof(buffer))
            break;
    }

    fclose(fp);
    printf("[S4] File stored successfully: %s\n", filepath);
    send(client_fd, "STORE_SUCCESS", 13, 0);
    return 0;
}

void handle_client(int client_fd) {
    char request_type[20];
    
    // First determine if this is a download request
    int len = recv(client_fd, request_type, sizeof(request_type) - 1, MSG_PEEK);
    if (len <= 0) {
        perror("[S4] Failed to determine request type");
        return;
    }
    request_type[len] = '\0';

    if (strncmp(request_type, "DOWNLOAD ", 9) == 0) {
        // Handle download request
        char request[BUFFER_SIZE];
        len = recv(client_fd, request, sizeof(request) - 1, 0);
        if (len <= 0) {
            perror("[S4] Failed to receive download request");
            return;
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
            perror("[S4] Failed to receive remove request");
            return;  // Changed from continue to return
        }
        request[len] = '\0';
    
        char *filepath = request + 7; // Skip "REMOVE " prefix
        char expanded_path[MAX_PATH];
        expand_path(filepath, expanded_path, sizeof(expanded_path));
    
        if (remove(expanded_path) == 0) {
            printf("[S4] Deleted file: %s\n", expanded_path);
            send(client_fd, "REMOVE_SUCCESS", 14, 0);
        } else {
            perror("[S4] Failed to delete file");
            send(client_fd, "REMOVE_FAILED", 13, 0);
        }
    }
        
    else if (strncmp(request_type, "LIST ", 5) == 0) {
        char expanded_path[MAX_PATH];
        expand_path("~s4/", expanded_path, sizeof(expanded_path)); // Always use root
        
        // Find files with relative paths from server root
        char cmd[2 * MAX_PATH];
        snprintf(cmd, sizeof(cmd), "find %s -type f -name \"*.zip\" -printf \"%%P\\n\" | sort", expanded_path);
        
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
        // Handle upload request
        handle_upload_request(client_fd);
    }
}