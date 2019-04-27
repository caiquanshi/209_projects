#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include "helper.h"

int main(int argc, char *argv[]) {

    if (argc != 7) {
        fprintf(stderr, "Usage: psort -n <number of processes> -f <inputfile> -o <outputfile>\n");
        exit(1);
    }
    int n_process;
    extern char *optarg;
    int ch;
    char *input_file, *output_file;
    while ((ch = getopt(argc, argv, "n:f:o:")) != -1) {
        switch(ch) {
  	        case 'n':
  	        n_process = strtol(optarg, NULL, 10);
  	        break;	    
            case 'f':
  	        input_file = optarg;
  	        break;
  	        case 'o':
      	    output_file = optarg;
      	    break;
         	default:
                  fprintf(stderr, "Usage: psort -n <number of processses> "
      		"-f <input file name> -o <output file name>\n");
      	    exit(1);
  	    }
    }
    FILE* fp = fopen(input_file, "rb");
    int pipe_fd[n_process][2];
    int size = get_file_size(input_file)/sizeof(struct rec);
    int status;
    int large_process_size = size/n_process +1;
    int small_process_size = size/n_process;
    
    for (int pid = 0; pid < n_process; pid++){
    	pipe(pipe_fd[pid]);
      	int res = fork();
      	if(res < 0){
          	perror("fork");
          	exit(1);
        }
      	int p_size;
      	if(pid < size % n_process){
      		  p_size = large_process_size;
      	}else{
      		  p_size = small_process_size;
      	}
      	if(res > 0){
    		for (int k = 0; k < p_size; k++){
          	struct rec record;
    		fread(&record, sizeof(struct rec), 1, fp);
          	write(pipe_fd[pid][1], &record, sizeof(struct rec));
    		}
    		if (close(pipe_fd[pid][1]) == -1) {
    		    perror("close");
    		    exit(1);
    	  	}
        }
      	if(res == 0){
        fclose(fp);
    		struct rec recs[p_size];
    		for (int j = 0; j < pid; j++)
    		{
    			if (close(pipe_fd[j][0]) == -1) {
          		perror("close");
          		exit(1);
          		}
    		}
    		for (int i = 0; i < p_size; i++){
    			 read(pipe_fd[pid][0], &(recs[i]), sizeof(struct rec));
    		}
    		if (close(pipe_fd[pid][0]) == -1) {
          		perror("close");
          		exit(1);
    			}
    		qsort(recs, p_size, sizeof(struct rec), compare_freq);
    		for (int i = 0; i < p_size; i++){
      			if (write(pipe_fd[pid][1], &(recs[i]), sizeof(struct rec)) == -1){
        				perror("write");
        				exit(1);
      			}
    		}
    		if (close(pipe_fd[pid][1]) == -1) {
          		perror("close");
          		exit(1);
        	}
        	exit(0);
        }
    }
    fclose(fp);
    struct rec res[size];
    struct rec to_merge[n_process];
    for (int i = 0; i < n_process; i++){
        if (wait(&status) == -1) {
            perror("wait");
            exit(1);
        }
    }
    for (int i = 0; i < n_process; i++){
        if (read(pipe_fd[i][0], &(to_merge[i]), sizeof(struct rec)) == -1){
        	perror("read");
          	exit(1);
        }
    }
    for (int i = 0; i < size; i++){
        int min_index = min(to_merge, n_process);
        res[i] = to_merge[min_index];
        int read_res = read(pipe_fd[min_index][0], &(to_merge[min_index]), sizeof(struct rec));
        if(read_res == -1){
            perror("read");
            exit(1);}
        if (read_res == 0)
        {
        	  to_merge[min_index].freq = -1;
        } 
    }
    for (int i = 0; i < n_process; i++)
    {
    	if (close(pipe_fd[i][0]) == -1) {
            perror("close");
            exit(1);
        }
    }
    FILE *ofp = fopen(output_file, "w");
    for (int i = 0; i < size; i++){
      	if (fwrite(&(res[i]), sizeof(struct rec), 1, ofp) == 0) {
      	    perror("fwrite");
      	    exit(1);}
    }
    fclose(ofp);
    return 0;
}
