#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <errno.h>
#include <libgen.h>

#define PORT 5077
#define S2_PORT 7082
#define S3_PORT 3032
#define S4_PORT 2022
#define MAX_CLIENTS 5
#define BUFFER_SIZE 1024
#define MAX_PATH 512

void prcclient(int client_fd);
void expand_path(const char *input_path, char *output_path, size_t size);
int forward_to_s3(const char *filename, const char *destination, const char *filepath);
int forward_to_s4(const char *filename, const char *destination, const char *filepath);


int main() {
    int server_fd, client_fd;
    struct sockaddr_in server_addr, client_addr;
    socklen_t addr_len = sizeof(client_addr);

    // Step 1: Create socket
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1) {
        perror("Socket creation failed.");
        exit(EXIT_FAILURE);
    }

    // Step 2: Allow port reuse (FIX for "Address already in use")
    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt(SO_REUSEADDR) failed");
        exit(EXIT_FAILURE);
    }

    // Step 3: Configure server address
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);

    // Step 4: Bind socket to port
    if (bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) == -1) {
        perror("Bind failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    // Step 5: Start listening
    if (listen(server_fd, MAX_CLIENTS) == -1) {
        perror("Listening failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    printf("S1 Server is listening on port %d...\n", PORT);

    while (1) {
        // Step 6: Accept client connection
        client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &addr_len);
        if (client_fd == -1) {
            perror("Accept failed");
            continue;
        }

        printf("Client connected, now forking the child process.\n");

        // Step 7: Fork to handle client
        pid_t pid = fork();
        if (pid == 0) { // Child process
            close(server_fd);
            prcclient(client_fd);
            close(client_fd);
            exit(0);
        } else if (pid > 0) { // Parent process
            close(client_fd);
        } else {
            perror("Fork failed");
            close(client_fd);
        }
    }

    close(server_fd);
    return 0;
}

// Function to expand paths like "~" and "~s1"
void expand_path(const char *input_path, char *output_path, size_t size) {
    if (input_path[0] == '~') {
        const char *home = getenv("HOME");
        if (!home) {
            fprintf(stderr, "Error: HOME environment variable not set.\n");
            exit(1);
        }
        
        if (strncmp(input_path, "~s1", 3) == 0) {
            snprintf(output_path, size, "%s/s1%s", home, input_path + 3);
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

int forward_to_s2(const char *filename, const char *destination, const char *filepath) {
    int s2_fd;
    struct sockaddr_in s2_addr;

    // Create socket to connect to S2
    s2_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (s2_fd == -1) {
        perror("Socket creation for S2 failed");
        return -1;
    }

    // Configure S2 address
    memset(&s2_addr, 0, sizeof(s2_addr));
    s2_addr.sin_family = AF_INET;
    s2_addr.sin_port = htons(S2_PORT);  // S2's port
    inet_pton(AF_INET, "127.0.0.1", &s2_addr.sin_addr);

    // Connect to S2
    if (connect(s2_fd, (struct sockaddr *)&s2_addr, sizeof(s2_addr)) == -1) {
        perror("Connection to S2 failed");
        close(s2_fd);
        return -1;
    }

    // Prepare destination path for S2 (replace ~s1 with ~s2)
    char expanded_dest[MAX_PATH];
    expand_path(destination, expanded_dest, sizeof(expanded_dest));
    
    // Replace "s1" with "s2" in the path
    char *s1_pos = strstr(expanded_dest, "s1");
    if (s1_pos) {
        memcpy(s1_pos, "s2", 2);
    }

    // Send destination path first
    if (send(s2_fd, expanded_dest, strlen(expanded_dest), 0) == -1) {
        perror("Failed to send destination path to S2");
        close(s2_fd);
        return -1;
    }

    // Small delay to ensure S2 receives the path first
    usleep(10000);

    // Send filename next
    if (send(s2_fd, filename, strlen(filename), 0) == -1) {
        perror("Failed to send filename to S2");
        close(s2_fd);
        return -1;
    }

    // Small delay to ensure S2 receives the filename
    usleep(10000);

    // Send file data
    FILE *fp = fopen(filepath, "rb");
    if (!fp) {
        perror("Failed to open file for S2 transfer");
        close(s2_fd);
        return -1;
    }

    char buffer[BUFFER_SIZE];
    int bytes_read;
    while ((bytes_read = fread(buffer, 1, sizeof(buffer), fp)) > 0) {
        if (send(s2_fd, buffer, bytes_read, 0) == -1) {
            perror("Failed to send file data to S2");
            fclose(fp);
            close(s2_fd);
            return -1;
        }
    }

    fclose(fp);
    close(s2_fd);
    return 0;
}
int forward_to_s3(const char *filename, const char *destination, const char *filepath)
{
    int s3_fd;
    struct sockaddr_in s3_addr;

    // Create socket to connect to S3
    s3_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (s3_fd == -1)
    {
        perror("Socket creation for S3 failed");
        return -1;
    }

    // Configure S3 address
    memset(&s3_addr, 0, sizeof(s3_addr));
    s3_addr.sin_family = AF_INET;
    s3_addr.sin_port = htons(S3_PORT);
    inet_pton(AF_INET, "127.0.0.1", &s3_addr.sin_addr);

    // Connect to S3
    if (connect(s3_fd, (struct sockaddr *)&s3_addr, sizeof(s3_addr)) == -1)
    {
        perror("Connection to S3 failed");
        close(s3_fd);
        return -1;
    }

    // Prepare destination path for S3 (replace ~s1 with ~s3)
    char s3_destination[MAX_PATH];
    strncpy(s3_destination, destination, MAX_PATH);
    char *s1_pos = strstr(s3_destination, "~s1");
    if (s1_pos)
    {
        memcpy(s1_pos, "~s3", 3);
    }

    // Send command to S3
    char s3_command[BUFFER_SIZE];
    snprintf(s3_command, sizeof(s3_command), "STORE_TXT %s %s", filename, s3_destination);
    if (send(s3_fd, s3_command, strlen(s3_command), 0) == -1)
    {
        perror("Failed to send command to S3");
        close(s3_fd);
        return -1;
    }

    // Send file data to S3
    FILE *fp = fopen(filepath, "rb");
    if (!fp)
    {
        perror("Failed to open file for S3 transfer");
        close(s3_fd);
        return -1;
    }

    char buffer[BUFFER_SIZE];
    int bytes_read;
    while ((bytes_read = fread(buffer, 1, sizeof(buffer), fp)) > 0)
    {
        if (send(s3_fd, buffer, bytes_read, 0) == -1)
        {
            perror("Failed to send file data to S3");
            fclose(fp);
            close(s3_fd);
            return -1;
        }
    }

    fclose(fp);

    // Wait for acknowledgment from S3
    char ack[20];
    int len = recv(s3_fd, ack, sizeof(ack) - 1, 0);
    if (len <= 0)
    {
        perror("Failed to receive acknowledgment from S3");
        close(s3_fd);
        return -1;
    }

    ack[len] = '\0';
    close(s3_fd);

    if (strcmp(ack, "STORE_SUCCESS") == 0)
    {
        return 0;
    }
    else
    {
        return -1;
    }
}


int forward_to_s4(const char *filename, const char *destination, const char *filepath)
{
    int s4_fd;
    struct sockaddr_in s4_addr;

    // Create socket to connect to S4
    s4_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (s4_fd == -1)
    {
        perror("Socket creation for S4 failed");
        return -1;
    }

    // Configure S4 address
    memset(&s4_addr, 0, sizeof(s4_addr));
    s4_addr.sin_family = AF_INET;
    s4_addr.sin_port = htons(S4_PORT);
    inet_pton(AF_INET, "127.0.0.1", &s4_addr.sin_addr);

    // Connect to S4
    if (connect(s4_fd, (struct sockaddr *)&s4_addr, sizeof(s4_addr)) == -1)
    {
        perror("Connection to S4 failed");
        close(s4_fd);
        return -1;
    }

    // Prepare destination path for S4 (replace ~s1 with ~s4)
    char s4_destination[MAX_PATH];
    strncpy(s4_destination, destination, MAX_PATH);
    char *s1_pos = strstr(s4_destination, "~s1");
    if (s1_pos)
    {
        memcpy(s1_pos, "~s4", 3);
    }

    // Send command to S4
    char s4_command[BUFFER_SIZE];
    snprintf(s4_command, sizeof(s4_command), "STORE_ZIP %s %s", filename, s4_destination);
    if (send(s4_fd, s4_command, strlen(s4_command), 0) == -1)
    {
        perror("Failed to send command to S4");
        close(s4_fd);
        return -1;
    }

    // Send file data to S4
    FILE *fp = fopen(filepath, "rb");
    if (!fp)
    {
        perror("Failed to open file for S4 transfer");
        close(s4_fd);
        return -1;
    }

    char buffer[BUFFER_SIZE];
    int bytes_read;
    while ((bytes_read = fread(buffer, 1, sizeof(buffer), fp)) > 0)
    {
        if (send(s4_fd, buffer, bytes_read, 0) == -1)
        {
            perror("Failed to send file data to S4");
            fclose(fp);
            close(s4_fd);
            return -1;
        }
    }

    fclose(fp);

    // Wait for acknowledgment from S4
    char ack[20];
    int len = recv(s4_fd, ack, sizeof(ack) - 1, 0);
    if (len <= 0)
    {
        perror("Failed to receive acknowledgment from S4");
        close(s4_fd);
        return -1;
    }

    ack[len] = '\0';
    close(s4_fd);

    if (strcmp(ack, "STORE_SUCCESS") == 0)
    {
        return 0;
    }
    else
    {
        return -1;
    }
}


int request_file_from_s2(const char *s1_path, char *file_data, size_t *file_size) {
    int s2_fd;
    struct sockaddr_in s2_addr;

    // Create socket to connect to S2
    s2_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (s2_fd == -1) {
        perror("Socket creation for S2 failed");
        return -1;
    }

    // Configure S2 address
    memset(&s2_addr, 0, sizeof(s2_addr));
    s2_addr.sin_family = AF_INET;
    s2_addr.sin_port = htons(S2_PORT);
    inet_pton(AF_INET, "127.0.0.1", &s2_addr.sin_addr);

    // Connect to S2
    if (connect(s2_fd, (struct sockaddr *)&s2_addr, sizeof(s2_addr)) == -1) {
        perror("Connection to S2 failed");
        close(s2_fd);
        return -1;
    }

    // Prepare the path for S2 (replace ~s1 with ~s2)
    char expanded_path[MAX_PATH];
    expand_path(s1_path, expanded_path, sizeof(expanded_path));
    
    // Replace "s1" with "s2" in the path
    char *s1_pos = strstr(expanded_path, "s1");
    if (s1_pos) {
        memcpy(s1_pos, "s2", 2);
    }

    // Send request type to S2
    char request[BUFFER_SIZE];
    snprintf(request, sizeof(request), "DOWNLOAD %s", expanded_path);
    if (send(s2_fd, request, strlen(request), 0) == -1) {
        perror("Failed to send download request to S2");
        close(s2_fd);
        return -1;
    }

    // Receive file data from S2
    *file_size = 0;
    int bytes_received;
    while ((bytes_received = recv(s2_fd, file_data + *file_size, BUFFER_SIZE, 0)) > 0) {
        *file_size += bytes_received;
        if (bytes_received < BUFFER_SIZE) break;
    }

    close(s2_fd);
    return (*file_size > 0) ? 0 : -1;
}

int request_file_from_s3(const char *s1_path, char *file_data, size_t *file_size) {
    int s3_fd;
    struct sockaddr_in s3_addr;

    // Create socket to connect to S3
    s3_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (s3_fd == -1) {
        perror("[S1] Socket creation for S3 failed");
        return -1;
    }

    // Configure S3 address
    memset(&s3_addr, 0, sizeof(s3_addr));
    s3_addr.sin_family = AF_INET;
    s3_addr.sin_port = htons(S3_PORT);
    inet_pton(AF_INET, "127.0.0.1", &s3_addr.sin_addr);

    // Connect to S3
    if (connect(s3_fd, (struct sockaddr *)&s3_addr, sizeof(s3_addr)) == -1) {
        perror("[S1] Connection to S3 failed");
        close(s3_fd);
        return -1;
    }

    // Prepare the path for S3 (replace ~s1 with ~s3)
    char expanded_path[MAX_PATH];
    expand_path(s1_path, expanded_path, sizeof(expanded_path));
    
    // Replace "s1" with "s3" in the path
    char *s1_pos = strstr(expanded_path, "s1");
    if (s1_pos) {
        memcpy(s1_pos, "s3", 2);
    }

    // Send request type to S3
    char request[BUFFER_SIZE];
    snprintf(request, sizeof(request), "DOWNLOAD %s", expanded_path);
    if (send(s3_fd, request, strlen(request), 0) == -1) {
        perror("[S1] Failed to send download request to S3");
        close(s3_fd);
        return -1;
    }

    // Receive file data from S3
    *file_size = 0;
    int bytes_received;
    while ((bytes_received = recv(s3_fd, file_data + *file_size, BUFFER_SIZE, 0)) > 0) {
        *file_size += bytes_received;
        if (*file_size >= 10 * BUFFER_SIZE) { // Prevent buffer overflow
            fprintf(stderr, "[S1] File too large for buffer\n");
            break;
        }
    }

    close(s3_fd);
    return (*file_size > 0) ? 0 : -1;
}
int request_file_from_s4(const char *s1_path, char *file_data, size_t *file_size) {
    int s4_fd;
    struct sockaddr_in s4_addr;

    // Create socket to connect to S4
    s4_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (s4_fd == -1) {
        perror("[S1] Socket creation for S4 failed");
        return -1;
    }

    // Configure S4 address
    memset(&s4_addr, 0, sizeof(s4_addr));
    s4_addr.sin_family = AF_INET;
    s4_addr.sin_port = htons(S4_PORT);
    inet_pton(AF_INET, "127.0.0.1", &s4_addr.sin_addr);

    // Connect to S4
    if (connect(s4_fd, (struct sockaddr *)&s4_addr, sizeof(s4_addr)) == -1) {
        perror("[S1] Connection to S4 failed");
        close(s4_fd);
        return -1;
    }

    // Prepare the path for S4 (replace ~s1 with ~s4)
    char expanded_path[MAX_PATH];
    expand_path(s1_path, expanded_path, sizeof(expanded_path));
    
    // Replace "s1" with "s4" in the path
    char *s1_pos = strstr(expanded_path, "s1");
    if (s1_pos) {
        memcpy(s1_pos, "s4", 2);
    }

    // Send request type to S4
    char request[BUFFER_SIZE];
    snprintf(request, sizeof(request), "DOWNLOAD %s", expanded_path);
    if (send(s4_fd, request, strlen(request), 0) == -1) {
        perror("[S1] Failed to send download request to S4");
        close(s4_fd);
        return -1;
    }

    // Receive file data from S4
    *file_size = 0;
    int bytes_received;
    while ((bytes_received = recv(s4_fd, file_data + *file_size, BUFFER_SIZE, 0)) > 0) {
        *file_size += bytes_received;
        if (*file_size >= 10 * BUFFER_SIZE) { // Prevent buffer overflow
            fprintf(stderr, "[S1] File too large for buffer\n");
            break;
        }
    }

    close(s4_fd);
    return (*file_size > 0) ? 0 : -1;
}

int request_remove_from_s2(const char *filepath) {
    int s2_fd;
    struct sockaddr_in s2_addr;

    // Create socket to connect to S2
    s2_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (s2_fd == -1) {
        perror("[S1] Socket creation for S2 failed");
        return -1;
    }

    // Configure S2 address
    memset(&s2_addr, 0, sizeof(s2_addr));
    s2_addr.sin_family = AF_INET;
    s2_addr.sin_port = htons(S2_PORT);
    inet_pton(AF_INET, "127.0.0.1", &s2_addr.sin_addr);

    // Connect to S2
    if (connect(s2_fd, (struct sockaddr *)&s2_addr, sizeof(s2_addr)) == -1) {
        perror("[S1] Connection to S2 failed");
        close(s2_fd);
        return -1;
    }

    // Prepare the path for S2 (replace ~s1 with ~s2)
    char expanded_path[MAX_PATH];
    expand_path(filepath, expanded_path, sizeof(expanded_path));
    char *s1_pos = strstr(expanded_path, "s1");
    if (s1_pos) {
        memcpy(s1_pos, "s2", 2);
    }

    // Send remove request
    char request[BUFFER_SIZE];
    snprintf(request, sizeof(request), "REMOVE %s", expanded_path);
    if (send(s2_fd, request, strlen(request), 0) == -1) {
        perror("[S1] Failed to send remove request to S2");
        close(s2_fd);
        return -1;
    }

    // Wait for response
    char response[20];
    int len = recv(s2_fd, response, sizeof(response) - 1, 0);
    if (len <= 0) {
        perror("[S1] Failed to receive response from S2");
        close(s2_fd);
        return -1;
    }
    response[len] = '\0';
    close(s2_fd);

    return (strcmp(response, "REMOVE_SUCCESS") == 0) ? 0 : -1;
}

int request_remove_from_s3(const char *filepath) {
    int s3_fd;
    struct sockaddr_in s3_addr;

    // Create socket to connect to S3
    s3_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (s3_fd == -1) {
        perror("[S1] Socket creation for S3 failed");
        return -1;
    }

    // Configure S3 address
    memset(&s3_addr, 0, sizeof(s3_addr));
    s3_addr.sin_family = AF_INET;
    s3_addr.sin_port = htons(S3_PORT);
    inet_pton(AF_INET, "127.0.0.1", &s3_addr.sin_addr);

    // Connect to S3
    if (connect(s3_fd, (struct sockaddr *)&s3_addr, sizeof(s3_addr)) == -1) {
        perror("[S1] Connection to S3 failed");
        close(s3_fd);
        return -1;
    }

    // Prepare the path for S3 (replace ~s1 with ~s3)
    char expanded_path[MAX_PATH];
    expand_path(filepath, expanded_path, sizeof(expanded_path));
    char *s1_pos = strstr(expanded_path, "s1");
    if (s1_pos) {
        memcpy(s1_pos, "s3", 2);
    }

    // Send remove request
    char request[BUFFER_SIZE];
    snprintf(request, sizeof(request), "REMOVE %s", expanded_path);
    if (send(s3_fd, request, strlen(request), 0) == -1) {
        perror("[S1] Failed to send remove request to S3");
        close(s3_fd);
        return -1;
    }

    // Wait for response
    char response[20];
    int len = recv(s3_fd, response, sizeof(response) - 1, 0);
    if (len <= 0) {
        perror("[S1] Failed to receive response from S3");
        close(s3_fd);
        return -1;
    }
    response[len] = '\0';
    close(s3_fd);

    return (strcmp(response, "REMOVE_SUCCESS") == 0) ? 0 : -1;
}

int request_remove_from_s4(const char *filepath) {
    int s4_fd;
    struct sockaddr_in s4_addr;

    // Create socket to connect to S3
    s4_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (s4_fd == -1) {
        perror("[S1] Socket creation for S3 failed");
        return -1;
    }

    // Configure S3 address
    memset(&s4_addr, 0, sizeof(s4_addr));
    s4_addr.sin_family = AF_INET;
    s4_addr.sin_port = htons(S4_PORT);
    inet_pton(AF_INET, "127.0.0.1", &s4_addr.sin_addr);

    // Connect to S3
    if (connect(s4_fd, (struct sockaddr *)&s4_addr, sizeof(s4_addr)) == -1) {
        perror("[S1] Connection to S3 failed");
        close(s4_fd);
        return -1;
    }

    // Prepare the path for S3 (replace ~s1 with ~s3)
    char expanded_path[MAX_PATH];
    expand_path(filepath, expanded_path, sizeof(expanded_path));
    char *s1_pos = strstr(expanded_path, "s1");
    if (s1_pos) {
        memcpy(s1_pos, "s4", 2);
    }

    // Send remove request
    char request[BUFFER_SIZE];
    snprintf(request, sizeof(request), "REMOVE %s", expanded_path);
    if (send(s4_fd, request, strlen(request), 0) == -1) {
        perror("[S1] Failed to send remove request to S4");
        close(s4_fd);
        return -1;
    }

    // Wait for response
    char response[20];
    int len = recv(s4_fd, response, sizeof(response) - 1, 0);
    if (len <= 0) {
        perror("[S1] Failed to receive response from S4");
        close(s4_fd);
        return -1;
    }
    response[len] = '\0';
    close(s4_fd);

    return (strcmp(response, "REMOVE_SUCCESS") == 0) ? 0 : -1;
}

int request_file_list_from_s2(const char *path, char *file_list, size_t *list_size) {
    int s2_fd;
    struct sockaddr_in s2_addr;

    s2_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (s2_fd == -1) {
        perror("[S1] Socket creation for S2 failed");
        return -1;
    }

    memset(&s2_addr, 0, sizeof(s2_addr));
    s2_addr.sin_family = AF_INET;
    s2_addr.sin_port = htons(S2_PORT);
    inet_pton(AF_INET, "127.0.0.1", &s2_addr.sin_addr);

    if (connect(s2_fd, (struct sockaddr *)&s2_addr, sizeof(s2_addr)) == -1) {
        perror("[S1] Connection to S2 failed");
        close(s2_fd);
        return -1;
    }

    // Prepare the path for S2 (replace ~s1 with ~s2)
    char expanded_path[MAX_PATH];
    expand_path(path, expanded_path, sizeof(expanded_path));
    char *s1_pos = strstr(expanded_path, "s1");
    if (s1_pos) {
        memcpy(s1_pos, "s2", 2);
    }

    // Send LIST request to S2
    char request[BUFFER_SIZE];
    snprintf(request, sizeof(request), "LIST %s", expanded_path);
    if (send(s2_fd, request, strlen(request), 0) == -1) {
        perror("[S1] Failed to send list request to S2");
        close(s2_fd);
        return -1;
    }

    // Receive file list from S2
    *list_size = 0;
    int bytes_received;
    while ((bytes_received = recv(s2_fd, file_list + *list_size, BUFFER_SIZE, 0)) > 0) {
        *list_size += bytes_received;
        if (bytes_received < BUFFER_SIZE) break;
    }

    close(s2_fd);
    return (*list_size > 0) ? 0 : -1;
}
int request_file_list_from_s3(const char *path, char *file_list, size_t *list_size) {
    int s3_fd;
    struct sockaddr_in s3_addr;

    s3_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (s3_fd == -1) {
        perror("[S1] Socket creation for S2 failed");
        return -1;
    }

    memset(&s3_addr, 0, sizeof(s3_addr));
    s3_addr.sin_family = AF_INET;
    s3_addr.sin_port = htons(S3_PORT);
    inet_pton(AF_INET, "127.0.0.1", &s3_addr.sin_addr);

    if (connect(s3_fd, (struct sockaddr *)&s3_addr, sizeof(s3_addr)) == -1) {
        perror("[S1] Connection to S3 failed");
        close(s3_fd);
        return -1;
    }

    // Prepare the path for S2 (replace ~s1 with ~s2)
    char expanded_path[MAX_PATH];
    expand_path(path, expanded_path, sizeof(expanded_path));
    char *s1_pos = strstr(expanded_path, "s1");
    if (s1_pos) {
        memcpy(s1_pos, "s3", 2);
    }

    // Send LIST request to S2
    char request[BUFFER_SIZE];
    snprintf(request, sizeof(request), "LIST %s", expanded_path);
    if (send(s3_fd, request, strlen(request), 0) == -1) {
        perror("[S1] Failed to send list request to S3");
        close(s3_fd);
        return -1;
    }

    // Receive file list from S2
    *list_size = 0;
    int bytes_received;
    while ((bytes_received = recv(s3_fd, file_list + *list_size, BUFFER_SIZE, 0)) > 0) {
        *list_size += bytes_received;
        if (bytes_received < BUFFER_SIZE) break;
    }

    close(s3_fd);
    return (*list_size > 0) ? 0 : -1;
}
int request_file_list_from_s4(const char *path, char *file_list, size_t *list_size) {
    int s4_fd;
    struct sockaddr_in s4_addr;

    s4_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (s4_fd == -1) {
        perror("[S1] Socket creation for S4 failed");
        return -1;
    }

    memset(&s4_addr, 0, sizeof(s4_addr));
    s4_addr.sin_family = AF_INET;
    s4_addr.sin_port = htons(S4_PORT);
    inet_pton(AF_INET, "127.0.0.1", &s4_addr.sin_addr);

    if (connect(s4_fd, (struct sockaddr *)&s4_addr, sizeof(s4_addr)) == -1) {
        perror("[S1] Connection to S2 failed");
        close(s4_fd);
        return -1;
    }

    // Prepare the path for S2 (replace ~s1 with ~s2)
    char expanded_path[MAX_PATH];
    expand_path(path, expanded_path, sizeof(expanded_path));
    char *s1_pos = strstr(expanded_path, "s1");
    if (s1_pos) {
        memcpy(s1_pos, "s4", 2);
    }

    // Send LIST request to S2
    char request[BUFFER_SIZE];
    snprintf(request, sizeof(request), "LIST %s", expanded_path);
    if (send(s4_fd, request, strlen(request), 0) == -1) {
        perror("[S1] Failed to send list request to S4");
        close(s4_fd);
        return -1;
    }

    // Receive file list from S2
    *list_size = 0;
    int bytes_received;
    while ((bytes_received = recv(s4_fd, file_list + *list_size, BUFFER_SIZE, 0)) > 0) {
        *list_size += bytes_received;
        if (bytes_received < BUFFER_SIZE) break;
    }

    close(s4_fd);
    return (*list_size > 0) ? 0 : -1;
}

void list_files_recursive(const char *base_path, const char *path, char *file_list, size_t *list_size) {
    DIR *dir;
    struct dirent *ent;
    char full_path[MAX_PATH];
    
    snprintf(full_path, sizeof(full_path), "%s/%s", base_path, path);
    
    if ((dir = opendir(full_path)) != NULL) {
        while ((ent = readdir(dir)) != NULL) {
            if (ent->d_type == DT_DIR) {
                // Skip . and .. directories
                if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
                    continue;
                }
                
                // Build relative path for subdirectory
                char subdir_path[MAX_PATH];
                if (strlen(path) == 0) {
                    snprintf(subdir_path, sizeof(subdir_path), "%s", ent->d_name);
                } else {
                    snprintf(subdir_path, sizeof(subdir_path), "%s/%s", path, ent->d_name);
                }
                
                // Recursively process subdirectories
                list_files_recursive(base_path, subdir_path, file_list, list_size);
            } else if (ent->d_type == DT_REG) {
                char *ext = strrchr(ent->d_name, '.');
                if (ext && strcmp(ext, ".c") == 0) {
                    // Include relative path if in subdirectory
                    if (strlen(path) == 0) {
                        int len = snprintf(file_list + *list_size, 
                                          BUFFER_SIZE * 10 - *list_size, 
                                          "%s\n", ent->d_name);
                        *list_size += len;
                    } else {
                        int len = snprintf(file_list + *list_size, 
                                          BUFFER_SIZE * 10 - *list_size, 
                                          "%s/%s\n", path, ent->d_name);
                        *list_size += len;
                    }
                }
            }
        }
        closedir(dir);
    }
}

int get_local_c_files(const char *path, char *file_list, size_t *list_size) {
    char expanded_path[MAX_PATH];
    expand_path(path, expanded_path, sizeof(expanded_path));
    
    *list_size = 0;
    list_files_recursive(expanded_path, "", file_list, list_size); // Start with empty relative path
    return 0;
}


// Function to handle client requests

void prcclient(int client_fd) {
    while (1) {
        char command[BUFFER_SIZE];
        char filename[256];
        char destination[256];

        memset(command, 0, sizeof(command));
        memset(filename, 0, sizeof(filename));
        memset(destination, 0, sizeof(destination));

        // Receive command from client
        int bytes_received = recv(client_fd, command, sizeof(command) - 1, 0);
        if (bytes_received <= 0) {
            if (bytes_received == 0) {
                printf("[S1] Client disconnected\n");
            } else {
                perror("[S1] recv failed");
            }
            break;
        }

        // Remove trailing newline (if any)
        command[strcspn(command, "\n")] = 0;


// ===== UPLOAD COMMAND =====
if (strncmp(command, "uploadf ", 8) == 0) {
    if (sscanf(command, "uploadf %s %s", filename, destination) != 2) {
        send(client_fd, "UPLOAD_FAILED:INVALID_FORMAT", 28, 0);
        continue;
    }

    char expanded_dest[MAX_PATH];
    expand_path(destination, expanded_dest, sizeof(expanded_dest));

    struct stat st;
    if (stat(expanded_dest, &st) == -1) {
        mkdir(expanded_dest, 0777);
    }

    char filepath[MAX_PATH];
    snprintf(filepath, sizeof(filepath), "%s/%s", expanded_dest, filename);

    FILE *fp = fopen(filepath, "wb");
    if (!fp) {
        send(client_fd, "UPLOAD_FAILED:FILE_OPEN_ERROR", 30, 0);
        continue;
    }

    char buffer[BUFFER_SIZE];
    while ((bytes_received = recv(client_fd, buffer, sizeof(buffer), 0)) > 0) {
        fwrite(buffer, 1, bytes_received, fp);
        if (bytes_received < sizeof(buffer)) break;
    }
    fclose(fp);

    // Handle forwarding
    char *ext = strrchr(filename, '.');
    if (ext != NULL) {
        if (strcmp(ext, ".pdf") == 0) {
            if (forward_to_s2(filename, destination, filepath) == 0) {
                remove(filepath);
                printf("[S1] PDF file forwarded to S2\n");
            }
        } else if (strcmp(ext, ".txt") == 0) {
            if (forward_to_s3(filename, destination, filepath) == 0) {
                remove(filepath);
                printf("[S1] TXT file forwarded to S3\n");
            }
        } else if (strcmp(ext, ".zip") == 0) {
            if (forward_to_s4(filename, destination, filepath) == 0) {
                remove(filepath);
                printf("[S1] ZIP file forwarded to S4\n");
            }
        } else {
            printf("[S1] .c file stored locally\n");
        }
    }

    send(client_fd, "UPLOAD_SUCCESS", 14, 0);
    continue;
}
// ===== DOWNLOAD COMMAND =====
else if (strncmp(command, "downlf ", 7) == 0) {
    char requested_file[MAX_PATH];
    sscanf(command + 7, "%s", requested_file);

    char expanded_path[MAX_PATH];
    expand_path(requested_file, expanded_path, sizeof(expanded_path));

    // Check file extension
    char *ext = strrchr(requested_file, '.');
    if (ext != NULL) {
        if (strcmp(ext, ".pdf") == 0) {
            // Handle PDF file - request from S2
            char file_data[10 * BUFFER_SIZE]; // Buffer for file content
            size_t file_size = 0;
            
            if (request_file_from_s2(requested_file, file_data, &file_size) == 0) {
                // Send file to client
                send(client_fd, file_data, file_size, 0);
                printf("[S1] PDF file retrieved from S2 and sent to client\n");
                continue;
            } else {
                send(client_fd, "DOWNLOAD_FAILED:FILE_NOT_FOUND_ON_S2", 35, 0);
                continue;
            }
        }
        //handle txt file
        else if (strcmp(ext, ".txt") == 0) {
            char file_data[10 * BUFFER_SIZE];
            size_t file_size = 0;
            
            if (request_file_from_s3(requested_file, file_data, &file_size) == 0) {
                send(client_fd, file_data, file_size, 0);
                printf("[S1] TXT file retrieved from S3 and sent to client\n");
                continue;
            } else {
                send(client_fd, "DOWNLOAD_FAILED:FILE_NOT_FOUND_ON_S3", 35, 0);
                continue;
            }
        }
        else if (strcmp(ext, ".zip") == 0) {
            char file_data[10 * BUFFER_SIZE];
            size_t file_size = 0;
            
            if (request_file_from_s4(requested_file, file_data, &file_size) == 0) {
                // First send success message to client
                send(client_fd, "DOWNLOAD_SUCCESS", 16, 0);
                // Then send the actual file data
                send(client_fd, file_data, file_size, 0);
                printf("[S1] ZIP file retrieved from S4 and sent to client\n");
            } else {
                send(client_fd, "DOWNLOAD_FAILED:FILE_NOT_FOUND_ON_S4", 35, 0);
            }
            continue;
        }
    }

    FILE *fp = fopen(expanded_path, "rb");
    if (!fp) {
        perror("[S1] Requested file not found");
        send(client_fd, "DOWNLOAD_FAILED", 15, 0);
        continue;
    }

    printf("[S1] Sending file to client: %s\n", expanded_path);
    char buffer[BUFFER_SIZE];
    int bytes_read;
    while ((bytes_read = fread(buffer, 1, sizeof(buffer), fp)) > 0) {
        send(client_fd, buffer, bytes_read, 0);
    }
    fclose(fp);
    continue;
}
// ===== REMOVE COMMAND =====
else if (strncmp(command, "removef ", 8) == 0) {
    char filepath[MAX_PATH];
    sscanf(command + 8, "%s", filepath);

    char expanded_path[MAX_PATH];
    expand_path(filepath, expanded_path, sizeof(expanded_path));

    // Check file extension
    char *ext = strrchr(filepath, '.');
    if (ext != NULL) {
        if (strcmp(ext, ".c") == 0) {
            // Handle .c file - delete locally
            if (remove(expanded_path) == 0) {
                printf("[S1] Deleted .c file: %s\n", expanded_path);
                send(client_fd, "REMOVE_SUCCESS", 14, 0);
            } else {
                perror("[S1] Failed to delete .c file");
                send(client_fd, "REMOVE_FAILED", 13, 0);
            }
        }
        else if (strcmp(ext, ".pdf") == 0) {
            // Handle PDF file - request S2 to delete
            if (request_remove_from_s2(filepath) == 0) {
                printf("[S1] Requested S2 to delete PDF: %s\n", expanded_path);
                send(client_fd, "REMOVE_SUCCESS", 14, 0);
            } else {
                printf("[S1] Failed to delete PDF via S2\n");
                send(client_fd, "REMOVE_FAILED", 13, 0);
            }
        }
        else if (strcmp(ext, ".txt") == 0) {
            // Handle TXT file - request S3 to delete
            if (request_remove_from_s3(filepath) == 0) {
                printf("[S1] Requested S3 to delete TXT: %s\n", expanded_path);
                send(client_fd, "REMOVE_SUCCESS", 14, 0);
            } else {
                printf("[S1] Failed to delete TXT via S3\n");
                send(client_fd, "REMOVE_FAILED", 13, 0);
            }
        }
        else if (strcmp(ext, ".zip") == 0) {
            // Handle PDF file - request S2 to delete
            if (request_remove_from_s4(filepath) == 0) {
                printf("[S4] Requested S4 to delete PDF: %s\n", expanded_path);
                send(client_fd, "REMOVE_SUCCESS", 14, 0);
            } else {
                printf("[S4] Failed to delete PDF via S2\n");
                send(client_fd, "REMOVE_FAILED", 13, 0);
            }
        }
        else {
            send(client_fd, "REMOVE_FAILED:INVALID_FILE_TYPE", 31, 0);
        }
    } else {
        send(client_fd, "REMOVE_FAILED:NO_EXTENSION", 27, 0);
    }
    continue;
}
// ===== DOWNLOAD TAR COMMAND =====

else if (strncmp(command, "downltar ", 9) == 0) {
    char filetype[10];
    sscanf(command + 9, "%s", filetype);
    
    if (strcmp(filetype, ".c") == 0) {
        // Get the home directory
        const char *home = getenv("HOME");
        if (!home) {
            perror("[S1] HOME environment variable not set");
            send(client_fd, "TAR_FAILED:NO_HOME", 18, 0);
            continue;
        }

        // Create a temporary tar file
        char tar_path[] = "/tmp/cfiles_XXXXXX.tar";
        int fd = mkstemps(tar_path, 4); // Creates unique temp file with .tar extension
        if (fd < 0) {
            perror("[S1] Failed to create temp tar file");
            send(client_fd, "TAR_FAILED:TEMP_FILE", 20, 0);
            continue;
        }
        close(fd); // We'll use the path with system() commands

        // Build the find and tar command
        char cmd[1024];
        snprintf(cmd, sizeof(cmd),
            "find %s/s1 -type f -name \"*.c\" -print0 | "
            "tar -cf %s --null -T - 2>/dev/null",
            home, tar_path);

        // Execute the command
        int ret = system(cmd);
        if (ret != 0) {
            perror("[S1] Failed to create tar file");
            unlink(tar_path); // Clean up
            send(client_fd, "TAR_FAILED:TAR_CREATE", 22, 0);
            continue;
        }

        // Open and send the tar file
        FILE *fp = fopen(tar_path, "rb");
        if (!fp) {
            perror("[S1] Failed to open tar file");
            unlink(tar_path);
            send(client_fd, "TAR_FAILED:FILE_OPEN", 21, 0);
            continue;
        }

        // First send the actual tar file
        char buffer[BUFFER_SIZE];
        size_t bytes_read;
        while ((bytes_read = fread(buffer, 1, sizeof(buffer), fp)) > 0) {
            if (send(client_fd, buffer, bytes_read, 0) < 0) {
                perror("[S1] Error sending tar file");
                break;
            }
        }
        fclose(fp);
        unlink(tar_path); // Clean up temp file

        shutdown(client_fd, SHUT_WR); // Signal end of transfer
        printf("[S1] Sent cfiles.tar to client\n");
    }
    else if (strcmp(filetype, ".pdf") == 0) {
        // Connect to S2 for PDF files
        int s2_fd = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in s2_addr;
        memset(&s2_addr, 0, sizeof(s2_addr));
        s2_addr.sin_family = AF_INET;
        s2_addr.sin_port = htons(S2_PORT);
        inet_pton(AF_INET, "127.0.0.1", &s2_addr.sin_addr);
    
        if (connect(s2_fd, (struct sockaddr *)&s2_addr, sizeof(s2_addr)) == -1) {
            perror("[S1] Connection to S2 failed");
            send(client_fd, "TAR_FAILED:S2_CONNECTION", 24, 0);
            continue;
        }
    
        // Send PDF tar request with explicit filename
        if (send(s2_fd, "TAR_PDF:pdfiles.tar", 19, 0) == -1) {
            perror("[S1] Failed to send request to S2");
            close(s2_fd);
            send(client_fd, "TAR_FAILED:S2_REQUEST", 22, 0);
            continue;
        }
    
        // Forward the tar file to client
        char buffer[BUFFER_SIZE];
        int bytes_received;
        while ((bytes_received = recv(s2_fd, buffer, sizeof(buffer), 0)) > 0) {
            if (send(client_fd, buffer, bytes_received, 0) == -1) {
                perror("[S1] Error forwarding PDF tar");
                break;
            }
        }
    
        close(s2_fd);
        shutdown(client_fd, SHUT_WR); // Signal end of transfer
        printf("[S1] PDF tar (pdfiles.tar) forwarded successfully\n");
    }
    else if (strcmp(filetype, ".txt") == 0) {
        // Connect to S3 for TXT files
        int s3_fd = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in s3_addr;
        memset(&s3_addr, 0, sizeof(s3_addr));
        s3_addr.sin_family = AF_INET;
        s3_addr.sin_port = htons(S3_PORT);
        inet_pton(AF_INET, "127.0.0.1", &s3_addr.sin_addr);
    
        if (connect(s3_fd, (struct sockaddr *)&s3_addr, sizeof(s3_addr)) == -1) {
            perror("[S1] Connection to S3 failed");
            send(client_fd, "TAR_FAILED:S3_CONNECTION", 24, 0);
            continue;
        }
    
        // Send TXT tar request with explicit filename
        if (send(s3_fd, "TAR_TXT:txtfiles.tar", 20, 0) == -1) {
            perror("[S1] Failed to send request to S3");
            close(s3_fd);
            send(client_fd, "TAR_FAILED:S3_REQUEST", 22, 0);
            continue;
        }
    
        // Forward the tar file to client
        char buffer[BUFFER_SIZE];
        int bytes_received;
        while ((bytes_received = recv(s3_fd, buffer, sizeof(buffer), 0)) > 0) {
            if (send(client_fd, buffer, bytes_received, 0) == -1) {
                perror("[S1] Error forwarding TXT tar");
                break;
            }
        }
    
        close(s3_fd);
        shutdown(client_fd, SHUT_WR); // Signal end of transfer
        printf("[S1] TXT tar (txtfiles.tar) forwarded successfully\n");
    }
    else {
        send(client_fd, "TAR_FAILED:UNSUPPORTED_TYPE", 28, 0);
    }
}

// ===== DISPLAY FILENAMES COMMAND =====
else if (strncmp(command, "dispfnames ", 11) == 0) {
    char path[MAX_PATH];
    sscanf(command + 11, "%s", path);
    
    char all_files[BUFFER_SIZE * 10] = {0};
    size_t total_size = 0;

    // 1. Get .c files from S1
    char s1_files[BUFFER_SIZE] = {0};
    size_t s1_size = 0;
    if (get_local_c_files(path, s1_files, &s1_size) == 0) {
        // Just copy the filenames without s1/ prefix
        strncat(all_files, s1_files, sizeof(all_files) - total_size - 1);
        total_size += s1_size;
    }

    // 2. Get .pdf files from S2
    char s2_files[BUFFER_SIZE] = {0};
    size_t s2_size = 0;
    if (request_file_list_from_s2(path, s2_files, &s2_size) == 0) {
        // Just copy the filenames without s2/ prefix
        strncat(all_files, s2_files, sizeof(all_files) - total_size - 1);
        total_size += s2_size;
    }

    // 3. Get .txt files from S3
    char s3_files[BUFFER_SIZE] = {0};
    size_t s3_size = 0;
    if (request_file_list_from_s3(path, s3_files, &s3_size) == 0) {
        // Just copy the filenames without s3/ prefix
        strncat(all_files, s3_files, sizeof(all_files) - total_size - 1);
        total_size += s3_size;
    }

    // 4. Get .zip files from S4
    char s4_files[BUFFER_SIZE] = {0};
    size_t s4_size = 0;
    if (request_file_list_from_s4(path, s4_files, &s4_size) == 0) {
        // Just copy the filenames without s4/ prefix
        strncat(all_files, s4_files, sizeof(all_files) - total_size - 1);
        total_size += s4_size;
    }

    // Sort the combined list alphabetically by file type (.c first, then .pdf, etc.)
    // This is already handled by the individual servers returning sorted lists
    // and the order we concatenate them (.c first, then .pdf, etc.)

    // Send the combined list to client
    send(client_fd, all_files, total_size, 0);
}

// ===== EXIT COMMAND =====
else if (strcmp(command, "exit") == 0) {
    send(client_fd, "GOODBYE", 7, 0);
    break;
}
// ===== INVALID COMMAND =====
else {
    send(client_fd, "INVALID_COMMAND", 16, 0);
}
    }
}
