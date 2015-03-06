################################################################################
# Automatically-generated file. Do not edit!
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
O_SRCS += \
../src/pgcluster/libpgc/SUBSYS.o \
../src/pgcluster/libpgc/dllist.o \
../src/pgcluster/libpgc/sem.o \
../src/pgcluster/libpgc/show.o \
../src/pgcluster/libpgc/signal.o 

C_SRCS += \
../src/pgcluster/libpgc/dllist.c \
../src/pgcluster/libpgc/sem.c \
../src/pgcluster/libpgc/show.c \
../src/pgcluster/libpgc/signal.c 

OBJS += \
./src/pgcluster/libpgc/dllist.o \
./src/pgcluster/libpgc/sem.o \
./src/pgcluster/libpgc/show.o \
./src/pgcluster/libpgc/signal.o 

C_DEPS += \
./src/pgcluster/libpgc/dllist.d \
./src/pgcluster/libpgc/sem.d \
./src/pgcluster/libpgc/show.d \
./src/pgcluster/libpgc/signal.d 


# Each subdirectory must supply rules for building sources it contributes
src/pgcluster/libpgc/%.o: ../src/pgcluster/libpgc/%.c
	@echo 'Building file: $<'
	@echo 'Invoking: GCC C Compiler'
	gcc -O0 -g3 -Wall -c -fmessage-length=0 -MMD -MP -MF"$(@:%.o=%.d)" -MT"$(@:%.o=%.d)" -o"$@" "$<"
	@echo 'Finished building: $<'
	@echo ' '


