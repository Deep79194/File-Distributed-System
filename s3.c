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

#define S3_PORT 3032
#define BUFFER_SIZE 1024
#define MAX_PATH 512

#define TAR_BUFFER_SIZE (10 * 1024 * 1024)  // 10MB buffer for tar files

void handle_client(int client_fd);
void expand_path(const char *input_path, char *output_path, size_t size);
int handle_download_request(int client_fd, const char *filepath);
int handle_upload_request(int client_fd);

int main()
{
    int server_fd, client_fd;
    struct sockaddr_in server_addr, client_addr;
    socklen_t addr_len = sizeof(client_addr);

    // Create socket
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1)
    {
        perror("[S3] Socket creation failed");
        exit(EXIT_FAILURE);
    }

    // Allow port reuse
    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
    {
        perror("[S3] setsockopt failed");
        exit(EXIT_FAILURE);
    }

    // Configure server address
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(S3_PORT);

    // Bind socket to port
    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1)
    {
        perror("[S3] Bind failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    // Start listening
    if (listen(server_fd, 5) == -1)
    {
        perror("[S3] Listening failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    printf("[S3] Server is listening on port %d for .txt files...\n", S3_PORT);

    while (1)
    {
        // Accept client connection (will be S1 in this case)
        client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &addr_len);
        if (client_fd == -1)
        {
            perror("[S3] Accept failed");
            continue;
        }

        // printf("[S3] S1 connected to S3\n");
        handle_client(client_fd);
        close(client_fd);
    }

    close(server_fd);
    return 0;
}

// Function to expand paths like "~" and "~s3"
void expand_path(const char *input_path, char *output_path, size_t size)
{
    if (input_path[0] == '~')
    {
        const char *home = getenv("HOME");
        if (!home)
        {
            fprintf(stderr, "[S3] Error: HOME environment variable not set.\n");
            exit(1);
        }

        if (strncmp(input_path, "~s3", 3) == 0)
        {
            snprintf(output_path, size, "%s/s3%s", home, input_path + 3);
        }
        else if (strcmp(input_path, "~") == 0)
        {
            strncpy(output_path, home, size);
        }
        else
        {
            char *slash = strchr(input_path, '/');
            if (slash)
            {
                snprintf(output_path, size, "%s%s", home, slash);
            }
            else
            {
                strncpy(output_path, home, size);
            }
        }
    }
    else
    {
        strncpy(output_path, input_path, size);
    }
}

// Function to handle download requests from S1
int handle_download_request(int client_fd, const char *filepath) {
    char expanded_path[MAX_PATH];
    expand_path(filepath, expanded_path, sizeof(expanded_path));

    FILE *fp = fopen(expanded_path, "rb");
    if (!fp) {
        printf("[S3] Requested TXT file not found: %s\n", expanded_path);
        send(client_fd, "FILE_NOT_FOUND", 14, 0);
        return -1;
    }

    printf("[S3] Sending TXT file: %s\n", expanded_path);
    char buffer[BUFFER_SIZE];
    int bytes_read, total_sent = 0;
    
    while ((bytes_read = fread(buffer, 1, sizeof(buffer), fp)) > 0) {
        int sent = send(client_fd, buffer, bytes_read, 0);
        if (sent == -1) {
            perror("[S3] Failed to send file data");
            fclose(fp);
            return -1;
        }
        total_sent += sent;
    }
    
    fclose(fp);
    printf("[S3] Sent %d bytes\n", total_sent);
    return 0;
}

// Function to handle upload requests from S1
int handle_upload_request(int client_fd) {
    char command[BUFFER_SIZE];
    char filename[256], destination[256], filepath[MAX_PATH], expanded_dest[MAX_PATH];

    // Receive command from S1 (format: "STORE_TXT <filename> <destination>")
    int bytes_received = recv(client_fd, command, sizeof(command) - 1, 0);
    if (bytes_received <= 0) {
        perror("[S3] Failed to receive command from S1");
        return -1;
    }
    command[bytes_received] = '\0';

    // Parse command
    if (sscanf(command, "STORE_TXT %s %s", filename, destination) != 2) {
        perror("[S3] Invalid command format from S1");
        send(client_fd, "STORE_FAILED", 12, 0);
        return -1;
    }

    // Expand destination path (convert ~s1 to ~s3)
    char modified_dest[MAX_PATH];
    strncpy(modified_dest, destination, MAX_PATH);
    char *s1_pos = strstr(modified_dest, "~s1");
    if (s1_pos) {
        memcpy(s1_pos, "~s3", 3);
    }

    expand_path(modified_dest, expanded_dest, sizeof(expanded_dest));

    // Create directory if it doesn't exist
    struct stat st;
    if (stat(expanded_dest, &st) == -1) {
        mkdir(expanded_dest, 0777);
    }

    // Construct full file path
    snprintf(filepath, sizeof(filepath), "%s/%s", expanded_dest, filename);

    printf("[S3] Receiving .txt file: %s, Destination: %s\n", filename, filepath);

    // Open file for writing
    FILE *fp = fopen(filepath, "wb");
    if (!fp) {
        perror("[S3] File open failed");
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
    printf("[S3] File stored successfully: %s\n", filepath);
    send(client_fd, "STORE_SUCCESS", 13, 0);
    return 0;
}


int create_txt_tar(const char *output_path) {
    char cmd[2 * MAX_PATH];
    snprintf(cmd, sizeof(cmd), "find ~/s3 -type f -name \"*.txt\" | tar -cf %s --files-from - 2>/dev/null", 
             output_path);
    
    int ret = system(cmd);
    if (ret != 0) {
        fprintf(stderr, "[S3] Failed to create TXT tar archive\n");
        return -1;
    }
    return 0;
}


int handle_tar_request(int client_fd, const char *requested_name) {
    // Create temporary directory for tar file
    char temp_dir[] = "/tmp/s3_tar_XXXXXX";
    if (mkdtemp(temp_dir) == NULL) {
        perror("[S3] Failed to create temp directory");
        return -1;
    }

    // Create full path for tar file
    char tar_path[MAX_PATH];
    snprintf(tar_path, sizeof(tar_path), "%s/%s", temp_dir, requested_name);

    // Create the TXT tar archive
    char cmd[2 * MAX_PATH];
    snprintf(cmd, sizeof(cmd), "find ~/s3 -type f -name \"*.txt\" -print0 | tar -cf %s --null -T - 2>/dev/null", 
             tar_path);
    
    int ret = system(cmd);
    if (ret != 0) {
        fprintf(stderr, "[S3] Failed to create TXT tar archive at %s\n", tar_path);
        // Clean up
        char cleanup_cmd[MAX_PATH + 50];
        snprintf(cleanup_cmd, sizeof(cleanup_cmd), "rm -rf %s", temp_dir);
        system(cleanup_cmd);
        return -1;
    }

    // Open and send the tar file
    FILE *fp = fopen(tar_path, "rb");
    if (!fp) {
        perror("[S3] Failed to open tar file");
        // Clean up
        char cleanup_cmd[MAX_PATH + 50];
        snprintf(cleanup_cmd, sizeof(cleanup_cmd), "rm -rf %s", temp_dir);
        system(cleanup_cmd);
        return -1;
    }

    // Send file content directly
    char buffer[BUFFER_SIZE];
    size_t bytes_read;
    while ((bytes_read = fread(buffer, 1, sizeof(buffer), fp)) > 0) {
        if (send(client_fd, buffer, bytes_read, 0) == -1) {
            perror("[S3] Error sending tar file");
            break;
        }
    }

    fclose(fp);
    
    // Clean up
    char cleanup_cmd[MAX_PATH + 50];
    snprintf(cleanup_cmd, sizeof(cleanup_cmd), "rm -rf %s", temp_dir);
    system(cleanup_cmd);
    
    return 0;
}

// Main client handling function
void handle_client(int client_fd) {
    char request_type[20];
    
    // First determine if this is a download request
    int len = recv(client_fd, request_type, sizeof(request_type) - 1, MSG_PEEK);
    if (len <= 0) {
        perror("[S3] Failed to determine request type");
        return;
    }
    request_type[len] = '\0';

    if (strncmp(request_type, "DOWNLOAD ", 9) == 0) {
        // Handle download request
        char request[BUFFER_SIZE];
        len = recv(client_fd, request, sizeof(request) - 1, 0);
        if (len <= 0) {
            perror("[S3] Failed to receive download request");
            return;
        }
        request[len] = '\0';

        char *filepath = request + 9; // Skip "DOWNLOAD " prefix
        handle_download_request(client_fd, filepath);
    } 
   

    else if (strncmp(request_type, "TAR_TXT:", 8) == 0) {
        // Handle TXT tar request with specific filename
        char request[BUFFER_SIZE];
        len = recv(client_fd, request, sizeof(request) - 1, 0);
        if (len <= 0) {
            perror("[S3] Failed to receive tar request");
            close(client_fd);
            return;
        }
        request[len] = '\0';
        
        // Extract the requested filename (format is "TAR_TXT:filename.tar")
        char *colon = strchr(request, ':');
        if (!colon) {
            printf("[S3] Invalid TAR_TXT request format\n");
            close(client_fd);
            return;
        }
        char *requested_name = colon + 1;
        
        if (handle_tar_request(client_fd, requested_name) == 0) {
            printf("[S3] Sent TXT tar archive '%s' to client\n", requested_name);
        } else {
            printf("[S3] Failed to create/send TXT tar archive\n");
        }
    }

    else if (strncmp(request_type, "REMOVE ", 7) == 0) {
        // Handle remove request
        char request[BUFFER_SIZE];
        len = recv(client_fd, request, sizeof(request) - 1, 0);
        if (len <= 0) {
            perror("[S3] Failed to receive remove request");
            return;  // Changed from continue to return
        }
        request[len] = '\0';
    
        char *filepath = request + 7; // Skip "REMOVE " prefix
        char expanded_path[MAX_PATH];
        expand_path(filepath, expanded_path, sizeof(expanded_path));
    
        if (remove(expanded_path) == 0) {
            printf("[S3] Deleted file: %s\n", expanded_path);
            send(client_fd, "REMOVE_SUCCESS", 14, 0);
        } else {
            perror("[S3] Failed to delete file");
            send(client_fd, "REMOVE_FAILED", 13, 0);
        }
    }    
    else if (strncmp(request_type, "LIST ", 5) == 0) {
        char expanded_path[MAX_PATH];
        expand_path("~s3/", expanded_path, sizeof(expanded_path)); // Always use root
        
        // Find files with relative paths from server root
        char cmd[2 * MAX_PATH];
        snprintf(cmd, sizeof(cmd), "find %s -type f -name \"*.txt\" -printf \"%%P\\n\" | sort", expanded_path);
        
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