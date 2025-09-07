// Client side implementation of UDP client-server model 
#include <bits/stdc++.h> 
#include <cmath>
#include <iostream>
#include <stdlib.h> 
#include <unistd.h> 
#include <string.h> 
#include <sys/types.h> 
#include <sys/socket.h> 
#include <arpa/inet.h> 
#include <netinet/in.h> 
#include <chrono>  // Required for std::chrono::milliseconds
  
#define PORT	8080 
#define MAXLINE 1024 
#define CHANNELS 128
  
// Driver code 
int main() { 
    int sockfd; 
    char buffer[MAXLINE]; 
    const char *hello = "Hello from client"; 
    struct sockaddr_in     servaddr; 
  
    // Creating socket file descriptor 
    if ( (sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0 ) { 
        perror("socket creation failed"); 
        exit(EXIT_FAILURE); 
    } 
  
    memset(&servaddr, 0, sizeof(servaddr)); 
      
    // Filling server information 
    servaddr.sin_family = AF_INET; 
    servaddr.sin_port = htons(PORT); 
    servaddr.sin_addr.s_addr = INADDR_ANY; 
      
    int n;
    socklen_t len; 

	int frame = 0;
	short f[CHANNELS];

	const float frequency = 0.25;
	const float period = (1.0 / frequency);

	while (1)
	{
		const float t = frame / 1000.0;
		const float scaled = fmodf(t * frequency, 1);
		for (int i = 0; i < CHANNELS; i++)
		{
			f[i] = (short) (sinf((float) 2 * M_PI * t * frequency + i * 0.06) * 32766);
		}

		sendto(sockfd, f, sizeof(short[CHANNELS]), 
			MSG_CONFIRM, (const struct sockaddr *) &servaddr,  
				sizeof(servaddr)); 

		std::this_thread::sleep_for(std::chrono::milliseconds(1));

		frame++;
	}
      
    
          
    
  
    close(sockfd); 
    return 0; 
}
