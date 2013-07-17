/*-----------------------------------------------------------------------------*/
/* Copyright: CodeChix Bay Area Chapter 2013                                   */
/*-----------------------------------------------------------------------------*/
#include "cc_pollthr_mgr.h"

adpoll_thread_mgr_t *
adp_thr_mgr_new(char *tname,
                uint32_t max_sockets,
                uint32_t max_pipes)
{
    int i = 0;
    char send_buf[256];
    adpoll_thread_mgr_t *this;
    adpoll_pollthr_data_t *thread_user_data;
    int tname_len;

    this = (adpoll_thread_mgr_t *)malloc(sizeof(adpoll_thread_mgr_t));
    
    thread_user_data = (adpoll_pollthr_data_t *)
        malloc(sizeof(adpoll_pollthr_data_t));

    while ((tname[i] != 0) && (i < (MAX_NAME_LEN - 2))) {
        this->tname[i] = tname[i];
        i++;
    }
    this->tname[i] = 0; /* NULL terminate */
    tname_len = i + 1;
    
    this->max_sockets = max_sockets;
    this->max_pipes = max_pipes;
    this->num_pipes = 0;

    this->pipes_arr = (int *)malloc(sizeof(int) * 2 * max_pipes);

    /* create pipe - read is in location 0 and write in 1 */
    if (pipe(this->pipes_arr) == -1) {
        CC_LOG_FATAL("%s(%d): pipe creation failed",__FUNCTION__,
                     __LINE__);
    }
    this->num_pipes += 2; /* pipe creates 2 fds */

    CC_LOG_DEBUG("%s(%d): pipe fds created: [%d][%d]",
                 __FUNCTION__, __LINE__,
                 this->pipes_arr[0], this->pipes_arr[1]);

    strncpy(thread_user_data->tname, this->tname, tname_len);
    thread_user_data->max_pollfds = max_sockets + max_pipes;
    thread_user_data->primary_pipe_rd_fd = this->pipes_arr[PRI_PIPE_RD_FD];
    thread_user_data->del_pipe_cv_mutex_p = &(this->del_pipe_cv_mutex);
    thread_user_data->del_pipe_cv_cond_p = &(this->del_pipe_cv_cond);
    
    this->thread_p = g_thread_new(this->tname,
                            (GThreadFunc) adp_thr_mgr_poll_thread_func,
                            thread_user_data);
    return this;
}

void adp_thr_mgr_free(adpoll_thread_mgr_t *this)
{
    int i;
    adpoll_thr_msg_t del_fd_msg;    
    /* wait for join */
    g_thread_join (this->thread_p);
    
    for(i=0; i < this->num_pipes; i++) {
        close(this->pipes_arr[i]);
    }
    
    free(this->pipes_arr);
    free(this);

}

/* Function: adp_thr_mgr_add_del_fd
 * API to create or remove an fd
 * The fd could be either a pipe or a network socket
 */
int
adp_thr_mgr_add_del_fd(adpoll_thread_mgr_t *this,
                       adpoll_thr_msg_t    *msg)
{
    int i, j, retval = -1;
    CC_LOG_DEBUG("%s(%d)", __FUNCTION__, __LINE__);
    if (msg->fd_type == PIPE) {
        if (msg->fd_action == ADD) {
            CC_LOG_DEBUG("%s(%d) pipe ADD", __FUNCTION__,
                         __LINE__);
            CC_LOG_DEBUG("%s(%d) this->num_pipes is %d",
                         __FUNCTION__, __LINE__, this->num_pipes);

            if (this->num_pipes == this->max_pipes) {
                CC_LOG_ERROR("%s(%d) unable to add more pipes - max out",
                             __FUNCTION__, __LINE__);
            }
            int new_rd_fd_index = this->num_pipes + RD_OFFSET;
            if (pipe(&this->pipes_arr[this->num_pipes]) == -1) {
                CC_LOG_FATAL("%s(%d): pipe creation failed",__FUNCTION__,
                             __LINE__);
            }
            CC_LOG_DEBUG("%s(%d): new pipes created. rd: %d  wr: %d",
                         __FUNCTION__, __LINE__,
                         this->pipes_arr[this->num_pipes],
                         this->pipes_arr[this->num_pipes + 1]);
            this->num_pipes += 2; /* pipe creates 2 fds */

            msg->fd = this->pipes_arr[new_rd_fd_index];
            
            write(this->pipes_arr[PRI_PIPE_WR_FD],
                  msg, sizeof(adpoll_thr_msg_t));

            retval = this->pipes_arr[this->num_pipes - 2 + WR_OFFSET];
            
        } else if (msg->fd_action == DELETE) {
            /* find this fd in pipes_arr */
            int del_rd_fd_index;
            gboolean found = FALSE;
            CC_LOG_DEBUG("%s(%d):pipe %d DELETE", __FUNCTION__, __LINE__,
                         msg->fd);
            if ((msg->fd == PRI_PIPE_RD_FD) ||
                (msg->fd == PRI_PIPE_WR_FD)) {

                /* Update the message to send to poll thr */
                msg->fd = this->pipes_arr[PRI_PIPE_RD_FD];
                
                CC_LOG_DEBUG("%s(%d): sending fd DEL to poll thr on fd %d",
                             __FUNCTION__, __LINE__,
                             this->pipes_arr[PRI_PIPE_WR_FD]);

                write(this->pipes_arr[PRI_PIPE_WR_FD],
                          msg, sizeof(adpoll_thr_msg_t));
                
                return retval;
            }

            for (i = 0; i < this->num_pipes; i++) {
                if (this->pipes_arr[i] == msg->fd) {
                    del_rd_fd_index = i;
                    if (IS_FD_WR(del_rd_fd_index)) {
                        del_rd_fd_index -= WR_OFFSET;
                    } else {
                        del_rd_fd_index -= RD_OFFSET;
                    }
                    found = TRUE;

                    CC_LOG_DEBUG("%s(%d): found the fd %d in pipes_arr",
                                 __FUNCTION__, __LINE__, this->pipes_arr[i]);

                    /* Update the message to send to poll thr */
                    msg->fd = this->pipes_arr[del_rd_fd_index];

                    /* TBD: change this to primary pipe */
                    /* process error return */
                    CC_LOG_DEBUG("%s(%d): sending fd DEL to poll thr on fd %d",
                                 __FUNCTION__, __LINE__,
                                 this->pipes_arr[PRI_PIPE_WR_FD]);
                    
                    write(this->pipes_arr[del_rd_fd_index + WR_OFFSET],
                          msg, sizeof(adpoll_thr_msg_t));

//                    write(this->pipes_arr[PRI_PIPE_WR_FD],
//                          msg, sizeof(adpoll_thr_msg_t));
                    
                    //cond wait for thread to delete the fds
                    g_mutex_lock(&this->del_pipe_cv_mutex);
                    g_cond_wait(&this->del_pipe_cv_cond,
                                &this->del_pipe_cv_mutex);
                    g_mutex_lock(&this->del_pipe_cv_mutex);
                    
                    close(this->pipes_arr[del_rd_fd_index]);
                    close(this->pipes_arr[del_rd_fd_index + WR_OFFSET]);
                          
                    for (j = del_rd_fd_index + 2; j < this->num_pipes; j++) {
                        this->pipes_arr[j-2] = this->pipes_arr[j];
                    }

                    this->num_pipes -= 2;
                    break;
                }
            }
        }
    }
    return retval;
}


void
fd_entry_free(adpoll_fd_info_t *data)
{
    free(data);
}

static void
poll_fd_process(adpoll_fd_info_t *data_p,
                char *tname)
{
    pollthr_private_t *thr_pvt_p = NULL;
    GList *fd_list;
    
    thr_pvt_p = g_private_get(&tname_key);
    fd_list = thr_pvt_p->fd_list;

    if ((data_p->pollfd_entry_p) &&
        ((data_p->pollfd_entry_p->revents & POLLIN) &
         (data_p->pollfd_entry_p->events & POLLIN)))
    {
        CC_LOG_DEBUG("%s(%d): POLLIN on fd %d",
                     __FUNCTION__, __LINE__, data_p->pollfd_entry_p->fd);
        if (data_p->pollin_func) {
            data_p->pollin_func(tname, (void *)data_p);
        }
    }
    if ((data_p->pollfd_entry_p) &&
        ((data_p->pollfd_entry_p->revents & POLLOUT) &
         (data_p->pollfd_entry_p->events & POLLOUT)))
    {
        CC_LOG_DEBUG("%s(%d): POLLOUT on fd %d",
                     __FUNCTION__, __LINE__, data_p->pollfd_entry_p->fd);
        if (data_p->pollout_func) {
            data_p->pollout_func(tname, (void *)data_p);
        }
    }
}

static void
print_fd_list(adpoll_fd_info_t *data_p)
{
    CC_LOG_INFO("fd: %d\tfd_type: %d\tpollin_func: %p\tpollout_func: %p",
                data_p->fd, data_p->fd_type, data_p->pollin_func,
                data_p->pollout_func);
}



/* Function: pollthr_pipe_process_func
 * Callback function to process a pipe read
 * This function is of type fd_process_func
 * additional user data is in pollin_user_data
 */
static void
pollthr_pipe_process_func(char *tname,
                          void *data)
{
    adpoll_fd_info_t *data_p = (adpoll_fd_info_t *)data;
    adpoll_thr_msg_t msg;
    adpoll_fd_info_t *fd_entry_p; /* append this entry to fd_list */
    int num_list_entries, i;
    struct pollfd *pollfd_entry_p;
    GList *traverse = NULL;
    
    pollthr_private_t *thr_pvt_p = NULL;
    GList *fd_list;
    
    thr_pvt_p = g_private_get(&tname_key);
    
    read(data_p->fd, &msg, sizeof(adpoll_thr_msg_t));
        
    CC_LOG_INFO("%s(%d): message received: fd type: %d,"
                " fd %d, fd action %d, poll events %d,"
                " in func: %p, out func: %p",
                __FUNCTION__, __LINE__,
                msg.fd_type, msg.fd, msg.fd_action,
                msg.poll_events, msg.pollin_func,
                msg.pollout_func);

    switch (msg.fd_action) {
      case ADD:
      {

          CC_LOG_DEBUG("%s(%d): pipe ADD %d of type %d of action %d",
                       __FUNCTION__, __LINE__, msg.fd, msg.fd_type,
                       msg.fd_action);
          fd_entry_p = (adpoll_fd_info_t *)malloc(sizeof(adpoll_fd_info_t));
          fd_entry_p->fd = msg.fd;
          fd_entry_p->fd_type = msg.fd_type;

          if (msg.fd_type == PIPE) {
              fd_entry_p->pollin_func = &pollthr_pipe_process_func;
              fd_entry_p->pollin_user_data = data_p->pollin_user_data;
              fd_entry_p->pollout_func = NULL;
              fd_entry_p->pollout_user_data = NULL;
          } else {
              fd_entry_p->pollin_func = msg.pollin_func;
              fd_entry_p->pollin_user_data = msg.pollin_user_data;
              fd_entry_p->pollout_func = msg.pollout_func;
              fd_entry_p->pollout_user_data = msg.pollout_user_data;
          }

          CC_LOG_DEBUG("%s(%d)[%s]: poll thr has %d pollfd entries",
                       __FUNCTION__, __LINE__, tname,
                       thr_pvt_p->num_pollfds);
          
          /* access and modify the polling thread's pollfd array */
          /* add a corresponding pollfd entry */
          pollfd_entry_p = &(thr_pvt_p->pollfd_arr[thr_pvt_p->num_pollfds]);

          thr_pvt_p->num_pollfds += 1;

          /* setup poll fd for primary pipe*/
          pollfd_entry_p->fd = msg.fd;
          pollfd_entry_p->events = msg.poll_events;

          fd_entry_p->pollfd_entry_p = pollfd_entry_p;
          
          thr_pvt_p->fd_list = g_list_append(thr_pvt_p->fd_list, fd_entry_p);
          g_private_replace(&tname_key,
                            (gpointer)thr_pvt_p);
          break;
          
      }
      case DELETE:
      {
          gboolean found = FALSE;
          int del_index;
          if (msg.fd_type == PIPE) {
              CC_LOG_DEBUG("%s(%d): pipe DELETE", __FUNCTION__, __LINE__);

              if (msg.fd == thr_pvt_p->pollfd_arr[0].fd) {
                  CC_LOG_DEBUG("%s(%d): Received DEL on primary read FD",
                               __FUNCTION__, __LINE__);

                  thr_pvt_p->num_pollfds = 0;
                  g_private_replace(&tname_key,
                                    (gpointer)thr_pvt_p);
                  return;
              }
                  
              /* find and delete the pollfd entry */              
              for (i = 1; i < thr_pvt_p->num_pollfds; i++) {
                  CC_LOG_DEBUG("%s(%d): thr_pvt pollfd iter %d is %d",
                               __FUNCTION__, __LINE__, i,
                               thr_pvt_p->pollfd_arr[i].fd);
                  if (msg.fd == thr_pvt_p->pollfd_arr[i].fd) {
                      del_index = i;
                      found = TRUE;
                      break;
                  }
              }
              if (found == TRUE) {
                  CC_LOG_DEBUG("%s(%d): found fd in pollfd_arr",
                               __FUNCTION__, __LINE__);
                  
                  for (i = del_index + 1 ; i< thr_pvt_p->num_pollfds; i++) {
                      thr_pvt_p->pollfd_arr[i-1].fd =
                          thr_pvt_p->pollfd_arr[i].fd;
                      
                      thr_pvt_p->pollfd_arr[i-1].events =
                          thr_pvt_p->pollfd_arr[i].events;
                      
                      thr_pvt_p->pollfd_arr[i-1].revents =
                          thr_pvt_p->pollfd_arr[i].revents;
                  }
                  thr_pvt_p->num_pollfds--;

                  /* find and delete the list entry for this pipe fd */
                  traverse = g_list_first(thr_pvt_p->fd_list);
                  fd_entry_p = NULL;
                  
                  while (traverse != NULL) {
                      if (((adpoll_fd_info_t *)(traverse->data))->fd == msg.fd) {
                          fd_entry_p = (adpoll_fd_info_t *)(traverse->data);
                          break;
                      }
                      traverse = g_list_next(traverse);
                  }
                  
                  if (fd_entry_p) {
                      thr_pvt_p->fd_list = g_list_remove(thr_pvt_p->fd_list,
                                                         (gconstpointer) fd_entry_p);
                  } else {

                      CC_LOG_ERROR("%s(%d)[%s]: inconsistent database "
                                   "- pollfd found but fd entry not in list",
                                   __FUNCTION__, __LINE__, tname);
                  }
                  
                  g_private_replace(&tname_key,
                                    (gpointer)thr_pvt_p);

                  //cond signal to delete the fds
                  g_mutex_lock(thr_pvt_p->del_pipe_cv_mutex_p);
                  g_cond_signal(thr_pvt_p->del_pipe_cv_cond_p);
                  g_mutex_unlock(thr_pvt_p->del_pipe_cv_mutex_p);
                  
              } else {
                  CC_LOG_ERROR("%s(%d): NOT found fd in pollfd_arr",
                               __FUNCTION__, __LINE__);
              }

          } else {
              /* socket delete processing */
          }
          break;
      }
      default:
        CC_LOG_FATAL("%s(%d): neither ADD nor DELETE",
                     __FUNCTION__, __LINE__);
    }
}


/*
 * Function: adp_thr_mgr_poll_thread_func
 * adp_thr_mgr_new() creates a poll thread that runs this function
 * This function polls on pipe and socket fds in a loop
 * The first pipe is setup permanently with the parent thread
 * Parent thread sends new socket and pipe fds to poll on via the
 *   permanent pipe.
 * The permanent pipe is deleted only when the poll thread is torn down
 */
void
adp_thr_mgr_poll_thread_func(adpoll_pollthr_data_t *pollthr_data_p)
{
    int rv, i;
    adpoll_thr_msg_t msg;
    adpoll_fd_info_t *fd_entry_p;
    pollthr_private_t *thr_pvt_p;
    char pollthr_name[MAX_NAME_LEN];

    if (pollthr_data_p == NULL) {
        CC_LOG_FATAL("%s(%d): received NULL user data", __FUNCTION__, __LINE__);
    }
    
    strncpy(pollthr_name, pollthr_data_p->tname, MAX_NAME_LEN);
    
    CC_LOG_DEBUG("%s(%d)[%s]: thread started with max pollfds %d"
                 " and primary pipe rd fd %d", __FUNCTION__,
                 __LINE__, pollthr_name,
                 pollthr_data_p->max_pollfds,
                 pollthr_data_p->primary_pipe_rd_fd);
    
    /* Initialize thread private data */
    thr_pvt_p = (pollthr_private_t *)malloc(sizeof(pollthr_private_t));
    thr_pvt_p->pollfd_arr = (struct pollfd *)malloc(sizeof(struct pollfd) *
                                                    pollthr_data_p->max_pollfds);
    thr_pvt_p->fd_list = NULL;

    thr_pvt_p->del_pipe_cv_mutex_p = pollthr_data_p->del_pipe_cv_mutex_p;
    thr_pvt_p->del_pipe_cv_cond_p = pollthr_data_p->del_pipe_cv_cond_p;

    /* Initialize the first fd entry in fd_list */
    fd_entry_p = (adpoll_fd_info_t *)malloc(sizeof(adpoll_fd_info_t));
    fd_entry_p->fd = pollthr_data_p->primary_pipe_rd_fd;
    fd_entry_p->fd_type = PIPE;
    fd_entry_p->pollin_func = &pollthr_pipe_process_func;
    fd_entry_p->pollin_user_data = pollthr_name;
    fd_entry_p->pollout_func = NULL;
    fd_entry_p->pollout_user_data = NULL;
        

    /* setup poll fd for primary pipe*/
    thr_pvt_p->pollfd_arr[0].fd = fd_entry_p->fd;
    thr_pvt_p->pollfd_arr[0].events = POLLIN;
    thr_pvt_p->num_pollfds = 1;

    fd_entry_p->pollfd_entry_p = &(thr_pvt_p->pollfd_arr[0]);
    thr_pvt_p->fd_list = g_list_append(thr_pvt_p->fd_list, fd_entry_p);


    g_private_set(&tname_key,
                  (gpointer)thr_pvt_p);

    free(pollthr_data_p);

    CC_LOG_DEBUG("%s(%d)[%s]: reading on fd %d",
                 __FUNCTION__, __LINE__, pollthr_name,
                 thr_pvt_p->pollfd_arr[0].fd);

    for( ; ; ) {
        CC_LOG_DEBUG("%s(%d)[%s] before poll",
                     __FUNCTION__, __LINE__,
                     pollthr_name);
        
        
        thr_pvt_p = g_private_get(&tname_key);

        if (thr_pvt_p->num_pollfds == 0) {
            /* self destruct */
            CC_LOG_DEBUG("%s(%d)[%s] num_pollfds ZERO. Self Destruct",
                         __FUNCTION__, __LINE__,
                         pollthr_name);
            
            break;
        }
        
        rv = poll(thr_pvt_p->pollfd_arr,
                  thr_pvt_p->num_pollfds,
                  10000);
        
        if (rv == -1) {
            CC_LOG_ERROR("%s(%d)[%s]: poll error %d",
                         __FUNCTION__, __LINE__,pollthr_name,
                         errno);
            break;
        } else if (rv == 0) {
            CC_LOG_DEBUG("%s(%d)[%s]:Timeout occurred! "
                         "No data after 10 seconds",
                         __FUNCTION__, __LINE__, pollthr_name);
            CC_LOG_DEBUG("%s(%d)[%s]: thread was polling on %d number of fds",
                         __FUNCTION__, __LINE__, pollthr_name,
                         thr_pvt_p->num_pollfds);
            for (i = 0; i < thr_pvt_p->num_pollfds; i++) {
                CC_LOG_DEBUG("thr_pvt_p->pollfd_arr[%d].fd: %d, "
                             "thr_pvt_p->pollfd_arr[%d].events: %d",
                             i, thr_pvt_p->pollfd_arr[i].fd,
                             i, thr_pvt_p->pollfd_arr[i].events);
            }
            break;
        } else {
            g_list_foreach(thr_pvt_p->fd_list, (GFunc)poll_fd_process,
                           (gpointer)pollthr_name);
            
            CC_LOG_DEBUG("%s(%d)[%s]: listing of updated GList",
                         __FUNCTION__, __LINE__, pollthr_name);
            g_list_foreach(thr_pvt_p->fd_list, (GFunc)print_fd_list, NULL);
            CC_LOG_DEBUG("%s(%d)[%s]: listing %d items of updated pollfd_arr",
                         __FUNCTION__, __LINE__, pollthr_name,
                         thr_pvt_p->num_pollfds);
            for (i = 0; i < thr_pvt_p->num_pollfds; i++) {
                CC_LOG_DEBUG("thr_pvt_p->pollfd_arr[%d].fd: %d, "
                             "thr_pvt_p->pollfd_arr[%d].events: %d",
                             i, thr_pvt_p->pollfd_arr[i].fd,
                             i, thr_pvt_p->pollfd_arr[i].events);
            }
        }
    }
/*TBD: free everything in the thread */
    free(thr_pvt_p->pollfd_arr);
    g_list_free_full(thr_pvt_p->fd_list, (GDestroyNotify)fd_entry_free);
    free(thr_pvt_p);
}

