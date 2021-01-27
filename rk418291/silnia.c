#include "cacti.h"
#include <stdio.h>

typedef unsigned long long num_t;

#define MSG_FACTORIZE	(message_type_t)1
#define MSG_SEND		(message_type_t)2

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

message_t message_factorize(size_t nbytes, void* data) {
	message_t msg;
	msg.message_type = MSG_FACTORIZE;
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
	(void)stateptr;
	(void)nbytes;
	(void)data;
}

void factorize(void **stateptr, size_t nbytes, void *data) {
	(void)stateptr;
	
	num_t** my_data = (num_t**)data;
	num_t* n = *my_data;
	num_t* k = *(my_data + 1);
	num_t* k_factorial = *(my_data + 2);

	role_t** role_data = (role_t**)data;
	role_t* role = *(role_data + 3);

	if (*k == *n) {
		*n = *(k_factorial);
	}
	else {
		*k = *k + 1;
		*k_factorial = (*k_factorial) * (*k);
		
		if (send_message(actor_id_self(), message_spawn(role)) != 0) {
			exit(-2);
		}
		
		if (send_message(actor_id_self(), message_send(nbytes, data)) != 0) {
			exit(-2);
		}
	}

	if (send_message(actor_id_self(), message_godie()) != 0) {
		exit(-2);
	}
}

void send(void** stateptr, size_t nbytes, void* data) {
	(void)stateptr;

	if (send_message(actor_id_self() + 1, message_factorize(nbytes, data)) != 0) {
		exit(-2);
	}
}

typedef void (* act_t2)(void** stateptr, size_t nbytes, void* data);

int main() {
	unsigned long long number;
	scanf("%lld", &number);

	if (number <= 0) {
		printf("%d\n", 1);
		return 0;
	}

	role_t role;
	role.nprompts = 3;

	act_t2* acts = (act_t2*)malloc(sizeof(act_t2) * role.nprompts);

	*(acts) = (act_t2)hello;
	*(acts + 1) = factorize;
	*(acts + 2) = send;

	role.prompts = (act_t*)acts;

	num_t n = (num_t)number;
	num_t k = 1;
	num_t k_factorial = 1;

	size_t nbytes = sizeof(role_t) + 3 * sizeof(num_t);
	void** data = malloc(nbytes);
	*data = &n;
	*(data + 1) = &k;
	*(data + 2) = &k_factorial;
	*(data + 3) = &role;

	actor_id_t a;

	if (actor_system_create(&a, &role) != 0)
		exit(-1);
	
	int check = send_message(a, message_factorize(nbytes, (void*)data));

	if (check != 0)
		exit(check);
	
	actor_system_join(a);
	
	printf("%llu\n", n);

	free(acts);
	free(data);

	return 0;
}
