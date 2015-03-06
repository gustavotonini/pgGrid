################################################################################
# Automatically-generated file. Do not edit!
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
O_SRCS += \
../src/pgcluster/pgrp/cascade.o \
../src/pgcluster/pgrp/conf.o \
../src/pgcluster/pgrp/lifecheck.o \
../src/pgcluster/pgrp/main.o \
../src/pgcluster/pgrp/pqformat.o \
../src/pgcluster/pgrp/recovery.o \
../src/pgcluster/pgrp/replicate.o \
../src/pgcluster/pgrp/rlog.o 

C_SRCS += \
../src/pgcluster/pgrp/cascade.c \
../src/pgcluster/pgrp/conf.c \
../src/pgcluster/pgrp/lifecheck.c \
../src/pgcluster/pgrp/main.c \
../src/pgcluster/pgrp/pqformat.c \
../src/pgcluster/pgrp/recovery.c \
../src/pgcluster/pgrp/replicate.c \
../src/pgcluster/pgrp/rlog.c 

OBJS += \
./src/pgcluster/pgrp/cascade.o \
./src/pgcluster/pgrp/conf.o \
./src/pgcluster/pgrp/lifecheck.o \
./src/pgcluster/pgrp/main.o \
./src/pgcluster/pgrp/pqformat.o \
./src/pgcluster/pgrp/recovery.o \
./src/pgcluster/pgrp/replicate.o \
./src/pgcluster/pgrp/rlog.o 

C_DEPS += \
./src/pgcluster/pgrp/cascade.d \
./src/pgcluster/pgrp/conf.d \
./src/pgcluster/pgrp/lifecheck.d \
./src/pgcluster/pgrp/main.d \
./src/pgcluster/pgrp/pqformat.d \
./src/pgcluster/pgrp/recovery.d \
./src/pgcluster/pgrp/replicate.d \
./src/pgcluster/pgrp/rlog.d 


# Each subdirectory must supply rules for building sources it contributes
src/pgcluster/pgrp/%.o: ../src/pgcluster/pgrp/%.c
	@echo 'Building file: $<'
	@echo 'Invoking: GCC C Compiler'
	gcc -O0 -g3 -Wall -c -fmessage-length=0 -MMD -MP -MF"$(@:%.o=%.d)" -MT"$(@:%.o=%.d)" -o"$@" "$<"
	@echo 'Finished building: $<'
	@echo ' '


