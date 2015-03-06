################################################################################
# Automatically-generated file. Do not edit!
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
O_SRCS += \
../src/pgcluster/pglb/child.o \
../src/pgcluster/pglb/cluster_table.o \
../src/pgcluster/pglb/lifecheck.o \
../src/pgcluster/pglb/load_balance.o \
../src/pgcluster/pglb/main.o \
../src/pgcluster/pglb/pool_auth.o \
../src/pgcluster/pglb/pool_connection_pool.o \
../src/pgcluster/pglb/pool_ip.o \
../src/pgcluster/pglb/pool_params.o \
../src/pgcluster/pglb/pool_process_query.o \
../src/pgcluster/pglb/pool_stream.o \
../src/pgcluster/pglb/ps_status.o \
../src/pgcluster/pglb/recovery.o \
../src/pgcluster/pglb/socket.o 

C_SRCS += \
../src/pgcluster/pglb/child.c \
../src/pgcluster/pglb/cluster_table.c \
../src/pgcluster/pglb/lifecheck.c \
../src/pgcluster/pglb/load_balance.c \
../src/pgcluster/pglb/main.c \
../src/pgcluster/pglb/pool_auth.c \
../src/pgcluster/pglb/pool_connection_pool.c \
../src/pgcluster/pglb/pool_ip.c \
../src/pgcluster/pglb/pool_params.c \
../src/pgcluster/pglb/pool_process_query.c \
../src/pgcluster/pglb/pool_stream.c \
../src/pgcluster/pglb/ps_status.c \
../src/pgcluster/pglb/recovery.c \
../src/pgcluster/pglb/socket.c 

OBJS += \
./src/pgcluster/pglb/child.o \
./src/pgcluster/pglb/cluster_table.o \
./src/pgcluster/pglb/lifecheck.o \
./src/pgcluster/pglb/load_balance.o \
./src/pgcluster/pglb/main.o \
./src/pgcluster/pglb/pool_auth.o \
./src/pgcluster/pglb/pool_connection_pool.o \
./src/pgcluster/pglb/pool_ip.o \
./src/pgcluster/pglb/pool_params.o \
./src/pgcluster/pglb/pool_process_query.o \
./src/pgcluster/pglb/pool_stream.o \
./src/pgcluster/pglb/ps_status.o \
./src/pgcluster/pglb/recovery.o \
./src/pgcluster/pglb/socket.o 

C_DEPS += \
./src/pgcluster/pglb/child.d \
./src/pgcluster/pglb/cluster_table.d \
./src/pgcluster/pglb/lifecheck.d \
./src/pgcluster/pglb/load_balance.d \
./src/pgcluster/pglb/main.d \
./src/pgcluster/pglb/pool_auth.d \
./src/pgcluster/pglb/pool_connection_pool.d \
./src/pgcluster/pglb/pool_ip.d \
./src/pgcluster/pglb/pool_params.d \
./src/pgcluster/pglb/pool_process_query.d \
./src/pgcluster/pglb/pool_stream.d \
./src/pgcluster/pglb/ps_status.d \
./src/pgcluster/pglb/recovery.d \
./src/pgcluster/pglb/socket.d 


# Each subdirectory must supply rules for building sources it contributes
src/pgcluster/pglb/%.o: ../src/pgcluster/pglb/%.c
	@echo 'Building file: $<'
	@echo 'Invoking: GCC C Compiler'
	gcc -O0 -g3 -Wall -c -fmessage-length=0 -MMD -MP -MF"$(@:%.o=%.d)" -MT"$(@:%.o=%.d)" -o"$@" "$<"
	@echo 'Finished building: $<'
	@echo ' '


