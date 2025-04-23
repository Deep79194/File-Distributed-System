#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <errno.h>          
#include <sys/time.h>       

#define SERVER_IP "127.0.0.1"
#define PORT 5077
#define BUFFER_SIZE 1024

void upload_file(int sockfd, char *filename, char *destination);
void download_file(int sockfd, char *filepath);

int main() {
    int sockfd;
    struct sockaddr_in server_addr;
    char command[BUFFER_SIZE], filename[256], destination[256];

    // Create socket
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd == -1) {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }

    // Server details
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    inet_pton(AF_INET, SERVER_IP, &server_addr.sin_addr);

    // Connect to server
    if (connect(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) == -1) {
        perror("Connection to S1 failed");
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    printf("Connected to S1.\n");

    while (1) {
        printf("Enter command (or 'exit' to quit):\n");
        fgets(command, sizeof(command), stdin);
        command[strcspn(command, "\n")] = 0;  // Remove newline

        // Exit condition
        if (strcmp(command, "exit") == 0) {
            printf("Closing client.\n");
            break;
        }

        // Parse and handle upload command
        if (sscanf(command, "uploadf %s %s", filename, destination) == 2) {
            send(sockfd, command, strlen(command), 0);  // Send upload command
            upload_file(sockfd, filename, destination);
        } 
        else if(sscanf(command, "downlf %s", filename) == 1) {
            send(sockfd, command, strlen(command), 0);
            download_file(sockfd, filename);
            
        }
        
else if (sscanf(command, "removef %s", filename) == 1) {
    send(sockfd, command, strlen(command), 0);
    
    // Wait for response
    char response[50];
    int resp_len = recv(sockfd, response, sizeof(response) - 1, 0);
    if (resp_len > 0) {
        response[resp_len] = '\0';
        printf("Server response: %s\n", response);
    } else {
        printf("No response received from server.\n");
    }
}

else if (sscanf(command, "downltar %s", filename) == 1) {
    send(sockfd, command, strlen(command), 0);
    
    // Determine correct output filename based on filetype
    char *output_name;
    if (strcmp(filename, ".c") == 0) {
        output_name = "cfiles.tar";
    } else if (strcmp(filename, ".pdf") == 0) {
        output_name = "pdfiles.tar"; 
    } else if (strcmp(filename, ".txt") == 0) {
        output_name = "txtfiles.tar";
    } else {
        printf("Error: Unsupported file type for tar download\n");
        continue;
    }
    
    download_file(sockfd, output_name);
}



else if (sscanf(command, "dispfnames %s", filename) == 1) {
    send(sockfd, command, strlen(command), 0);
    
    // Receive and display the file list
    char file_list[BUFFER_SIZE * 10]; // Large buffer for file list
    int bytes_received = recv(sockfd, file_list, sizeof(file_list) - 1, 0);
    if (bytes_received > 0) {
        file_list[bytes_received] = '\0';
        printf("Files in %s:\n%s\n", filename, file_list);
    } else {
        printf("No files found or error receiving file list.\n");
    }
}
        else {
            printf("Invalid command format! Use: uploadf <filename> <destination>\n");
        }
    }

    close(sockfd);
    return 0;
}

void upload_file(int sockfd, char *filename, char *destination) {
    char buffer[BUFFER_SIZE];
    FILE *fp = fopen(filename, "rb");

    if (!fp) {
        perror("File open failed");
        return;
    }

    printf("Uploading %s...\n", filename);

    int bytes_read;
    while ((bytes_read = fread(buffer, 1, sizeof(buffer), fp)) > 0) {
        send(sockfd, buffer, bytes_read, 0);
    }

    fclose(fp);
    printf("File upload complete!\n");

    // Receive acknowledgment only
    memset(buffer, 0, sizeof(buffer));
    int ack_len = recv(sockfd, buffer, sizeof(buffer) - 1, 0);
    if (ack_len > 0) {
        buffer[ack_len] = '\0';
        printf("Server response: %s\n", buffer);
    } else {
        printf("No acknowledgment received.\n");
    }
}

// ========== Download ==========
void download_file(int sockfd, char *filepath) {
    char *slash = strrchr(filepath, '/');
    const char *filename = (slash != NULL) ? slash + 1 : filepath;

    FILE *fp = fopen(filename, "wb");
    if (!fp) {
        perror("Failed to create local file");
        return;
    }

    char buffer[BUFFER_SIZE];
    int bytes_received;
    printf("Downloading %s...\n", filename);

    while ((bytes_received = recv(sockfd, buffer, sizeof(buffer), 0)) > 0) {
        fwrite(buffer, 1, bytes_received, fp);
        if (bytes_received < BUFFER_SIZE) break;
    }

    fclose(fp);
    printf("Downloaded %s successfully.\n", filename);
}
