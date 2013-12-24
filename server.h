/*
 * server.h
 *
 *  Created on: 22 בדצמ 2013
 *      Author: ASUS
 */

#ifndef SERVER_H_
#define SERVER_H_

#include "client.h"

#define MAX_NAME_LEN 1000

typedef struct client_node {
	int pid;
	char name[MAX_NAME_LEN];
	int name_len;
	int fifo_in;
	int fifo_out;
	Queue to_send;
	pthread_t t_fifo_out;
	pthread_t t_fifo_in;
	struct client_node *next;
}*Client_Node;

typedef struct list {
	int size;
	Client_Node head;
	int num_senders;
	pthread_cond_t senders_cond;
	int num_updaters;
	pthread_cond_t updaters_cond;
	pthread_mutex_t list_lock;

}*List;

typedef struct client_and_msg {
	message_t msg;
	Client_Node client;
} Client_and_Msg;

void read_lock(List list) {
	pthread_mutex_lock(&list->list_lock);
	while (list->num_updaters > 0) {
		pthread_cond_wait(&list->senders_cond, &list->list_lock);
	}
	list->num_senders++;
	pthread_mutex_unlock(&list->list_lock);

}

void write_lock(List list) {
	pthread_mutex_lock(&list->list_lock);
	while (list->num_senders > 0 || list->num_updaters > 0) {
		pthread_cond_wait(&list->updaters_cond, &list->list_lock);
	}
	list->num_updaters++;
	pthread_mutex_unlock(&list->list_lock);

}

void read_unlock(List list) {
	pthread_mutex_lock(&list->list_lock);
	list->num_senders--;
	if (list->num_senders == 0)
		pthread_cond_signal(&list->updaters_cond);
	pthread_mutex_unlock(&list->list_lock);
}

void write_unlock(List list) {
	pthread_mutex_lock(&list->list_lock);
	list->num_updaters--;
	if (list->num_updaters == 0) {
		pthread_cond_signal(&list->senders_cond);
		pthread_cond_broadcast(&list->senders_cond);
	}
	pthread_mutex_unlock(&list->list_lock);
}

List init_list(void) {
	List list = (List)(malloc(sizeof(*list)));
	list->head = NULL;
	list->num_senders = 0;
	list->num_updaters = 0;
	list->size = 0;
	pthread_cond_init(&list->senders_cond, NULL);
	pthread_cond_init(&list->updaters_cond, NULL);
	pthread_mutex_init(&list->list_lock, NULL);
}

Client_Node init_client_from_server(int pid, char* name) {
	Client_Node client = (Client_Node) malloc(sizeof(*client));
	if (!client)
		return NULL;

	snprintf(client->name, MAX_NAME_LEN, "%s", name);
	char temp[MAX_NAME_LEN];
	snprintf(temp, MAX_NAME_LEN, "fifo-%d-in", pid);
	client->fifo_in = open(temp, O_WRONLY);
	snprintf(temp, MAX_NAME_LEN, "fifo-%d-out", pid);
	client->fifo_out = open(temp, O_RDONLY);
	if (client->fifo_in == -1 || client->fifo_out == -1)
		return NULL;
	client->pid = pid;
	client->next = NULL;
	client->name_len = strlen(client->name);
	Queue q = init_queue();
	client->to_send = q;
	if(pthread_create(client->t_fifo_out,NULL,ts_fifo_out,&client->fifo_out) != 0){
		printf("init_client_from_server cant make thread\n");
		return NULL;
	}
	if(pthread_create(client->t_fifo_in,NULL,ts_fifo_in,client) != 0){
		printf("init_client_from_server cant make thread\n");
		return NULL;
	}
	return client;
}
void destroy_client_from_server(Client_Node client) {

}

Client_Node list_find_by_pid(List list, int pid) {
	read_lock(list);
	Client_Node temp = list->head;
	while (temp) {
		if (temp->pid == pid) {
			read_unlock(list);
			return temp;
		}
		temp = temp->next;
	}
	read_unlock(list);
	return NULL;
}

Client_Node list_find_by_name(List list, char* name) {
	read_lock(list);
	Client_Node temp = list->head;
	int i;
	while (temp) {
		if (!strcmp(temp->name, name)) {
			read_unlock(list);
			return temp;
		}
		temp = temp->next;
	}
	read_unlock(list);
	return NULL;
}

int list_add(List list, Client_Node client) {
	if (list_find_by_name(list, client->name)) {
		return 0;
	}
	write_lock(list);
	Client_Node head = list->head;
	client->next = head;
	list->head = client;
	list->size++;
	write_unlock(list);
	return 1;
}

int list_remove(List list, Client_Node client) {
	write_lock(list);
	Client_Node prev = list->head;
	Client_Node curr = list->head;
	while (curr) {
		if (curr == client) {
			if (curr == list->head) {
				curr = list->head->next;
				destroy_client_from_server(list->head);
				list->head = curr;
				break;
			}
			prev->next = curr->next;
			destroy_client_from_server(curr);
		}
		prev = curr;
		curr = curr->next;
	}
	list->size--;
	write_unlock(list);
	return 1;
}

void* enqueue_to_client(void* client_and_msg) {
	Client_and_Msg * c_and_m = (Client_and_Msg *) client_and_msg;
	enqueue(c_and_m->client->to_send, c_and_m->msg);
	return NULL;
}

void send_broadcast_msg(List list, message_t msg) {
	read_lock(list);
	pthread_t *threads = (pthread_t*) malloc(sizeof(*threads) * list->size);
	if (threads == NULL) {
		printf("Bad\n");
	}
	Client_Node temp = list->head;
	Client_and_Msg client_and_msg;
	int i = 0;
	while (temp) {
		client_and_msg.msg = msg;
		client_and_msg.client = temp;
		if (pthread_create(&threads[i], NULL, enqueue_to_client,
				&client_and_msg) != 0) {
			printf("broadcast_msg cant make thread\n");
			exit(-100);
		}
		i++;
		temp = temp->next;
	}
	int j = 0;
	for (j = 0; j < list->size; j++) {
		if (pthread_join(threads[j], NULL) != 0) {
			printf("broadcast_msg cant wait for thread\n");
			exit(-101);
		}
	}
	free(threads);
	read_unlock(list);
}

void send_private_msg(List list, Client_Node from, Client_Node to,
		message_t msg) {
	read_lock(list);
	pthread_t threads[2];
	Client_and_Msg client_and_msg = { msg, from };
	if (pthread_create(&threads[0], NULL, enqueue_to_client, &client_and_msg)
			!= 0) {
		printf("send_private_msg cant make thread\n");
		exit(-102);
	}
	client_and_msg.client = to;
	if (pthread_create(&threads[1], NULL, enqueue_to_client, &client_and_msg)
			!= 0) {
		printf("send_private_msg cant make thread\n");
		exit(-103);
	}
	read_unlock(list);
}
void handle_leave(List list, Client_Node client){
	message_t msg = { 11, "/leave_ack" };
	Client_and_Msg c_and_m = { msg, client };
	enqueue_to_client(&c_and_m);
	pthread_join(client->t_fifo_in,NULL);
	list_remove(list,client);
}
void handle_who(List list, Client_Node client){
	read_lock(list);
	Client_Node temp = list->head;
	Client_and_Msg c_and_m;
	message_t msg;
	while(temp){
		snprintf(msg.buffer,MAX_LENGTH,"who: %s",temp->name);
		msg.length = strlen(msg.buffer);
		c_and_m.msg = msg;
		c_and_m.client = client;
		enqueue_to_client(c_and_m);
		temp = temp->next;
	}
	read_unlock(list);
}
void handle_commands(List list, Client_Node client, Command cmd) {
	switch (cmd) {
	case LEAVE: handle_leave(list,client); break;
	case HISTORY: break; // TODO
	case WHO: handle_who(list,client); break;
	}
}

#endif /* SERVER_H_ */
