#include "cacti.h"
#include <stdio.h>

typedef long long num_t;

#define MSG_SUM 	(message_type_t)1
#define MSG_SEND	(message_type_t)2

message_t message_spawn(role_t* role) {
	message_t msg;
	msg.message_type = MSG_SPAWN;
	msg.nbytes = sizeof(role->nprompts) + sizeof(role->prompts);
	msg.data = role;
	return msg;
}

message_t message_godie() {
	message_t msg;
	msg.message_type = MSG_GODIE;
	return msg;
}

message_t message_hello(size_t nbytes, void* data) {
	message_t msg;
	msg.message_type = MSG_HELLO;
	msg.nbytes = nbytes;
	msg.data = data;
	return msg;
}

message_t message_sum(size_t nbytes, void* data) {
	message_t msg;
	msg.message_type = MSG_SUM;
	msg.nbytes = nbytes;
	msg.data = data;
	return msg;
}

message_t message_send(size_t nbytes, void* data) {
	message_t msg;
	msg.message_type = MSG_SEND;
	msg.nbytes = nbytes;
	msg.data = data;
	return msg;
}


void hello(void** stateptr, size_t nbytes, void* data) {
	*stateptr = malloc(sizeof(int) * 2);
	*((actor_id_t*)(*stateptr)) = *(actor_id_t*)(data) + 1;
	*((actor_id_t*)(*stateptr + 1)) = 0;
}

void sum(void **stateptr, size_t nbytes, void *data) {
	actor_id_t* my_col = (actor_id_t*)(*stateptr);
	actor_id_t* my_row = (actor_id_t*)(*stateptr + 1);

	num_t* k = *((num_t**)data);
	num_t* n = *((num_t**)(data + 1));
	num_t* values = *((num_t**)(data + 2));
	num_t* times = *((num_t**)(data + 3));
	role_t* role = *((role_t**)(data + 4));
	num_t* sums = *((num_t**)(data + 5));

	if (*my_row == 0 && *my_col != *n - 1) {
		if (send_message(actor_id_self(), message_spawn(role)) != 0) {
			exit(-2);
		}
	}
	
	num_t value = *(values + *my_row * *n + *my_col);
	num_t time = *(times + *my_row * *n + *my_col);
	num_t* result = sums + *my_row;

	usleep(time * 1000);

	*result = *result + value;

	if (*my_col != *n - 1) {
		if (send_message(actor_id_self() + 1, message_sum(nbytes, data)) != 0) {
			exit(-2);
		}
	}

	*my_row = *my_row + 1;

	if (*my_row == *k) {
		free(*stateptr);
		if (send_message(actor_id_self(), message_godie()) != 0) {
			exit(-2);
		}
	}
	else if (*my_col == 0) {
		if (send_message(actor_id_self(), message_sum(nbytes, data)) != 0) {
			exit(-2);
		}
	}
}

void send(void** stateptr, size_t nbytes, void* data) {
	if (send_message(actor_id_self() + 1, message_sum(nbytes, data)) != 0) {
		exit(-2);
	}
}


typedef void (* act_t2)(void** stateptr, size_t nbytes, void* data);

int main() {
	num_t k, n; // k - no of rows, n - no of columns
	scanf("%lld %lld", &k, &n);

	role_t role;
	role.nprompts = 3;
	act_t2* acts = (act_t2*)malloc(sizeof(act_t2) * role.nprompts);
	*(acts) = (act_t2)hello;
	*(acts + 1) = (act_t2)sum;
	*(acts + 2) = (act_t2)send;
	role.prompts = (act_t*)acts;

	num_t* values = (num_t*)malloc(sizeof(num_t) * k * n);
	num_t* times = (num_t*)malloc(sizeof(num_t) * k * n);
	num_t* sums = (num_t*)malloc(sizeof(num_t) * k);

	for (num_t row = 0; row < k; ++row) {
		for (num_t col = 0; col < n; ++col) {
			scanf("%lld %lld", values + row * n + col, times + row * n + col);
		}
	}

	size_t nbytes = sizeof(num_t) * 2 + sizeof(num_t) * k * n * 2 + sizeof(role_t) + sizeof(num_t) * k;
	void** data = (void**)malloc(nbytes);
	*data = &k;
	*(data + 1) = &n;
	*(data + 2) = values;
	*(data + 3) = times;
	*(data + 4) = &role;
	*(data + 5) = sums;

	actor_id_t a;

	if (actor_system_create(&a, &role) != 0)
		exit(-1);
	
	actor_id_t before_first_id = a - 1;

	int check = send_message(a, message_hello(sizeof(actor_id_t), (void*)&before_first_id));

	if (check != 0)
		exit(check);
	
	check = send_message(a, message_sum(nbytes, (void*)data));
	
	if (check != 0)
		exit(check);

	actor_system_join(a);
	
	for (num_t i = 0; i < k; ++i) {
		printf("%lld\n", *(sums + i));
	}

	free(acts);
	free(values);
	free(times);
	free(sums);
	free(data);

	return 0;
}
