################################################################################
# Automatically-generated file. Do not edit!
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
O_SRCS += \
../src/pgcluster/tool/bwmeasure.o \
../src/pgcluster/tool/pgcbench.o 

C_SRCS += \
../src/pgcluster/tool/bwmeasure.c \
../src/pgcluster/tool/pgcbench.c 

OBJS += \
./src/pgcluster/tool/bwmeasure.o \
./src/pgcluster/tool/pgcbench.o 

C_DEPS += \
./src/pgcluster/tool/bwmeasure.d \
./src/pgcluster/tool/pgcbench.d 


# Each subdirectory must supply rules for building sources it contributes
src/pgcluster/tool/%.o: ../src/pgcluster/tool/%.c
	@echo 'Building file: $<'
	@echo 'Invoking: GCC C Compiler'
	gcc -O0 -g3 -Wall -c -fmessage-length=0 -MMD -MP -MF"$(@:%.o=%.d)" -MT"$(@:%.o=%.d)" -o"$@" "$<"
	@echo 'Finished building: $<'
	@echo ' '


