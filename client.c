#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#define SERVER_IP "127.0.0.1"
#define PORT 8080
#define BUFFER_SIZE 2048

int current_user_id = -1;
int current_role_id = -1; // 1=Customer, 2=Employee, 3=Manager, 4=Admin

void send_request(const char *request_message, char *response) {
    int sock_fd;
    struct sockaddr_in server_addr;
    ssize_t bytes_read;
    
    if ((sock_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) { perror("socket failed"); return; }
    server_addr.sin_family = AF_INET; server_addr.sin_port = htons(PORT);
    if (inet_pton(AF_INET, SERVER_IP, &server_addr.sin_addr) <= 0) { 
        perror("Address not supported"); close(sock_fd); return;
    }

    if (connect(sock_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        strcpy(response, "ERROR|Connection refused. Is server running?");
        close(sock_fd);
        return;
    }
    
    send(sock_fd, request_message, strlen(request_message), 0);
    bytes_read = recv(sock_fd, response, BUFFER_SIZE - 1, 0);
    if (bytes_read > 0) {
        response[bytes_read] = '\0';
    } else {
        strcpy(response, "ERROR|Server closed connection.");
    }

    close(sock_fd);
}

void clear_input_buffer() {
    int c;
    while ((c = getchar()) != '\n' && c != EOF);
}

int get_int_input(const char *prompt) {
    int val;
    printf("%s", prompt);
    if (scanf(" %d", &val) == 1) { 
        clear_input_buffer(); 
        return val;
    }
    clear_input_buffer();
    return -1;
}

void customer_menu() {
    int choice;
    char request[BUFFER_SIZE];
    char response[BUFFER_SIZE];
    char amount_str[50], target_acc_id_str[50], new_pass[50], feedback[256];
    char *token;

    while (current_user_id != -1) {
        printf("\n--- Customer Menu (ID: %d) ---\n", current_user_id); 
        printf("1. View Account Balance\n"); 
        printf("2. Deposit Money\n");
        printf("3. Withdraw Money\n");
        printf("4. Transfer Funds\n");
        printf("5. Apply for a Loan\n");
        printf("6. Change Password\n");
        printf("7. View Transaction History\n");
        printf("8. Adding Feedback\n");
        printf("9. Logout\n");
        printf("10. Exit\n");
        
        choice = get_int_input("Enter choice: "); 

        switch (choice) {
            case 1: snprintf(request, BUFFER_SIZE, "VIEW_BALANCE|%d|%d", current_role_id, current_user_id); break;
            case 2: printf("Enter amount to deposit: "); scanf("%s", amount_str); clear_input_buffer(); 
                    snprintf(request, BUFFER_SIZE, "DEPOSIT|%d|%d|%s", current_role_id, current_user_id, amount_str); break;
            case 3: printf("Enter amount to withdraw: "); scanf("%s", amount_str); clear_input_buffer();
                    snprintf(request, BUFFER_SIZE, "WITHDRAW|%d|%d|%s", current_role_id, current_user_id, amount_str); break;
            case 4: printf("Enter target account ID: "); scanf("%s", target_acc_id_str); 
                    printf("Enter amount to transfer: "); scanf("%s", amount_str); clear_input_buffer();
                    snprintf(request, BUFFER_SIZE, "TRANSFER|%d|%d|%s|%s", current_role_id, current_user_id, target_acc_id_str, amount_str); break;
            case 5: printf("Enter loan amount: "); scanf("%s", amount_str); clear_input_buffer();
                    snprintf(request, BUFFER_SIZE, "APPLY_LOAN|%d|%d|%s", current_role_id, current_user_id, amount_str); break;
            case 6: printf("Enter new password: "); scanf("%s", new_pass); clear_input_buffer();
                    snprintf(request, BUFFER_SIZE, "CHANGE_PASS|%d|%d|%s", current_role_id, current_user_id, new_pass); break;
            case 7: snprintf(request, BUFFER_SIZE, "VIEW_HISTORY|%d|%d", current_role_id, current_user_id); break;
            case 8: printf("Enter feedback (max 255 chars): "); fgets(feedback, 255, stdin); feedback[strcspn(feedback, "\n")] = 0;
                    snprintf(request, BUFFER_SIZE, "ADD_FEEDBACK|%d|%d|%s", current_role_id, current_user_id, feedback); break;
	    case 9: 
                snprintf(request, BUFFER_SIZE, "LOGOUT|%d|%d", current_role_id, current_user_id);
                send_request(request, response);
                
                
                current_user_id = -1; 
                current_role_id = -1; 
                printf("Logged out successfully.\n"); 
                
                sleep(1); 
                return;
            case 10: exit(0);
            default: printf("Invalid choice.\n"); continue;
        }

        send_request(request, response);
        token = strtok(response, "|");
        if (token && strcmp(token, "SUCCESS") == 0) {
            printf(">> SUCCESS: ");
            token = strtok(NULL, "");
            if (token) printf("%s\n", token);
        } else {
            printf(">> ERROR: ");
            token = strtok(NULL, "");
            if (token) printf("%s\n", token);
        }
    }
}

void employee_menu() {
    int choice;
    char request[BUFFER_SIZE];
    char response[BUFFER_SIZE];
    char username[50], password[50], balance_str[50], target_id_str[50], new_pass[50];
    char *token;

    while (current_user_id != -1) {
        printf("\n--- Employee Menu (ID: %d) ---\n", current_user_id);
        printf("1. Add New Customer\n"); 
        printf("2. Modify Customer Details (Change Password)\n"); 
        printf("3. Process Loan Applications (Approve/Reject)\n"); 
        printf("4. View Customer Transactions (Passbook)\n"); 
        printf("5. View Assigned Loan Applications (Check Loan ID Here)\n"); 
        printf("6. Change Password (Self)\n"); 
        printf("7. Logout\n"); 
        printf("8. Exit\n"); 
        choice = get_int_input("Enter choice: ");

        switch(choice) {
            case 1: 
                printf("Enter new Customer Username: "); scanf("%s", username); clear_input_buffer();
                printf("Enter new Password: "); scanf("%s", password); clear_input_buffer();
                printf("Enter Initial Balance: "); scanf("%s", balance_str); clear_input_buffer();
                // ADD_CUSTOMER|2|EMP_ID|USERNAME|PASSWORD|INITIAL_BALANCE
                snprintf(request, BUFFER_SIZE, "ADD_CUSTOMER|%d|%d|%s|%s|%s",
                         current_role_id, current_user_id, username, password, balance_str);
                break;

            case 2: 
                printf("Enter Customer User ID to modify: "); scanf("%s", target_id_str); clear_input_buffer();
                printf("Enter NEW Password: "); scanf("%s", new_pass); clear_input_buffer();
                // MOD_CUSTOMER|2|EMP_ID|TARGET_USER_ID|NEW_PASS
                snprintf(request, BUFFER_SIZE, "MOD_CUSTOMER|%d|%d|%s|%s",
                         current_role_id, current_user_id, target_id_str, new_pass);
                break;

	    case 3: 
                printf("Enter Loan ID to process: "); scanf("%s", target_id_str); clear_input_buffer();
                printf("Enter status (APPROVED/REJECTED): "); 
                
                scanf("%s", new_pass); clear_input_buffer(); 
                
                snprintf(request, BUFFER_SIZE, "PROCESS_LOAN|%d|%d|%s|%s",
                         current_role_id, current_user_id, target_id_str, new_pass);
                break;

            case 4: 
                printf("Enter Customer User ID for Passbook: "); scanf("%s", target_id_str); clear_input_buffer();
                snprintf(request, BUFFER_SIZE, "VIEW_PASSBOOK|%d|%d|%s",
                         current_role_id, current_user_id, target_id_str);
                break;

            case 5: 
                printf("Fetching assigned loans...\n");
                snprintf(request, BUFFER_SIZE, "VIEW_ASSIGNED_LOANS|%d|%d",
                         current_role_id, current_user_id);
                break;

            case 6: 
                printf("Enter new password: "); scanf("%s", new_pass); clear_input_buffer();
                snprintf(request, BUFFER_SIZE, "CHANGE_PASS|%d|%d|%s",
                         current_role_id, current_user_id, new_pass);
                break;

	    case 7:
		snprintf(request, BUFFER_SIZE, "LOGOUT|%d|%d", current_role_id, current_user_id);
                send_request(request, response);
                
                current_user_id = -1; 
                current_role_id = -1; 
                printf("Logged out.\n"); 
		sleep(1);
                return; 

            case 8: exit(0);
            default: printf("Invalid choice.\n"); continue;
        }

        send_request(request, response);
        token = strtok(response, "|");
        if (token && strcmp(token, "SUCCESS") == 0) {
            printf(">> SUCCESS: ");
            token = strtok(NULL, "");
            if (token) {
                if (choice == 4) {
                    printf("\n=================================================================================\n");
                    printf("Timestamp\t\t| Type\t\t| Amount\t| Details\n");
                    printf("\n=================================================================================\n");
                    printf("%s\n", token);
		}else if (choice == 5) { 
                    printf("\n==================================================\n");
                    printf("%s\n", token);
                } else {
                    printf("%s\n", token);
                }
            }
        } else {
            printf(">> ERROR: ");
            token = strtok(NULL, "");
            if (token) printf("%s\n", token);
        }
    }
}

void manager_menu() {
    int choice;
    char request[BUFFER_SIZE];
    char response[BUFFER_SIZE];
    char target_id_str[50], employee_id_str[50], status_str[50], new_pass[50];
    char *token;

    while (current_user_id != -1) {
        printf("\n--- Manager Menu (ID: %d) ---\n", current_user_id);
        printf("1. Activate/Deactivate Customer Accounts\n"); 
        printf("2. Assign Loan Application Processes to Employees\n"); 
        printf("3. Review Customer Feedback\n"); 
        printf("4. Change Password\n"); 
        printf("5. Logout\n"); 
        printf("6. Exit\n"); 
        choice = get_int_input("Enter choice: ");

        switch (choice) {
            case 1: 
                printf("Enter Customer User ID: "); scanf("%s", target_id_str); clear_input_buffer();
                printf("Enter new status (ACTIVATE/DEACTIVATE): "); scanf("%s", status_str); clear_input_buffer();
                // Protocol: ACT_DEACT_ACC|3|MGR_ID|TARGET_USER_ID|STATUS
                snprintf(request, BUFFER_SIZE, "ACT_DEACT_ACC|%d|%d|%s|%s",
                         current_role_id, current_user_id, target_id_str, status_str);
                break;
                
            case 2: 
		printf("\n--- Fetching Pending Loans ---\n");
                snprintf(request, BUFFER_SIZE, "VIEW_LOANS|%d|%d", current_role_id, current_user_id);
                send_request(request, response);

                token = strtok(response, "|");
                if (token && strcmp(token, "SUCCESS") == 0) {
                    printf(">> Loan Status List:\n");
                    token = strtok(NULL, "");
                    if (token) printf("%s\n", token);
                } else {
                    printf(">> ERROR: Failed to retrieve loan list: %s\n", token ? strtok(NULL, "") : "Unknown.");
                    break; // Exit assignment if listing fails
                }

                printf("\n--- Loan Assignment ---\n");
                printf("Enter Loan ID to assign: "); scanf("%s", target_id_str); clear_input_buffer();
                printf("Enter Employee ID: "); scanf("%s", employee_id_str); clear_input_buffer();
                snprintf(request, BUFFER_SIZE, "ASSIGN_LOAN|%d|%d|%s|%s",
                         current_role_id, current_user_id, target_id_str, employee_id_str);

                break;
                
            case 3: 
                snprintf(request, BUFFER_SIZE, "REVIEW_FEEDBACK|%d|%d", current_role_id, current_user_id);
                break;
                
            case 4: 
                printf("Enter new password: "); scanf("%s", new_pass); clear_input_buffer();
                snprintf(request, BUFFER_SIZE, "CHANGE_PASS|%d|%d|%s",
                         current_role_id, current_user_id, new_pass);
                break;
	    case 5: 
                snprintf(request, BUFFER_SIZE, "LOGOUT|%d|%d", current_role_id, current_user_id);
                send_request(request, response); 
                
                current_user_id = -1; 
                current_role_id = -1; 
                printf("Logged out.\n"); 
                sleep(1); // Crucial delay
                return; // Exit the menu loop

            case 6: // Exit
                snprintf(request, BUFFER_SIZE, "LOGOUT|%d|%d", current_role_id, current_user_id);
                send_request(request, response); 
                
                current_user_id = -1; 
                current_role_id = -1;
                
                printf("Logged out.\n");
                sleep(1); // Crucial delay
                exit(0);

            default: printf("Invalid choice.\n"); continue;
        }

        send_request(request, response);
        token = strtok(response, "|");
        if (token && strcmp(token, "SUCCESS") == 0) {
            printf(">> SUCCESS: ");
            token = strtok(NULL, "");
            if (token) {
                if (choice == 3) {
                    printf("\n==================================================\n");
                    printf("%s\n", token);
                } else {
                    printf("%s\n", token);
                }
            }
        } else {
            printf(">> ERROR: ");
            token = strtok(NULL, "");
            if (token) printf("%s\n", token);
        }
    }
}

void administrator_menu() {
    int choice;
    char request[BUFFER_SIZE];
    char response[BUFFER_SIZE];
    char username[50], password[50], target_id_str[50], new_pass[50], new_role_id_str[50];
    char *token;

    while (current_user_id != -1) {
        printf("\n--- Administrator Menu (ID: %d) ---\n", current_user_id);
        printf("1. Add New Bank Employee\n"); 
        printf("2. Modify Customer/Employee Details\n"); 
        printf("3. Manage User Roles\n"); 
        printf("4. Change Password\n"); 
        printf("5. Logout\n"); 
        printf("6. Exit\n"); 
        choice = get_int_input("Enter choice: ");

        switch (choice) {
            case 1: 
                printf("Enter new Employee Username: "); scanf("%s", username); clear_input_buffer();
                printf("Enter new Password: "); scanf("%s", password); clear_input_buffer();
                snprintf(request, BUFFER_SIZE, "ADD_EMPLOYEE|%d|%d|%s|%s",
                         current_role_id, current_user_id, username, password);
                break;

            case 2: 
                printf("Enter Target User ID: "); scanf("%s", target_id_str); clear_input_buffer();
                printf("Enter NEW Password: "); scanf("%s", new_pass); clear_input_buffer();
                snprintf(request, BUFFER_SIZE, "MOD_USER_DETAILS|%d|%d|%s|%s",
                         current_role_id, current_user_id, target_id_str, new_pass);
                break;

            case 3: // Manage User Roles (MANAGE_ROLES)
                printf("Enter Target User ID: "); scanf("%s", target_id_str); clear_input_buffer();
                printf("Enter NEW Role ID (1=Cust, 2=Emp, 3=Mgr, 4=Adm): "); scanf("%s", new_role_id_str); clear_input_buffer();
                snprintf(request, BUFFER_SIZE, "MANAGE_ROLES|%d|%d|%s|%s",
                         current_role_id, current_user_id, target_id_str, new_role_id_str);
                break;

            case 4: // Change Password (Self) (CHANGE_PASS)
                printf("Enter new password: "); scanf("%s", new_pass); clear_input_buffer();
                snprintf(request, BUFFER_SIZE, "CHANGE_PASS|%d|%d|%s",
                         current_role_id, current_user_id, new_pass);
                break;

	    case 5: // Logout
                snprintf(request, BUFFER_SIZE, "LOGOUT|%d|%d", current_role_id, current_user_id);
                send_request(request, response);
                
                current_user_id = -1; 
                current_role_id = -1; 
                printf("Logged out.\n"); 
                sleep(1); // Crucial delay for session cleanup
                return;

            case 6: // Exit
                snprintf(request, BUFFER_SIZE, "LOGOUT|%d|%d", current_role_id, current_user_id);
                send_request(request, response);
                
                current_user_id = -1; 
                current_role_id = -1;
                
                printf("Logged out.\n");
                sleep(1);
                exit(0);
	    default: printf("Invalid choice.\n"); continue;
        }
	send_request(request, response);
        token = strtok(response, "|");
        if (token && strcmp(token, "SUCCESS") == 0) {
            printf(">> SUCCESS: ");
            token = strtok(NULL, "");
            if (token) printf("%s\n", token);
        } else {
            printf(">> ERROR: ");
            token = strtok(NULL, "");
            if (token) printf("%s\n", token);
        }
    }
}


int main() {
    int role_choice;
    char username[50], password[50];
    char request[BUFFER_SIZE];
    char response[BUFFER_SIZE];
    char *token;

    while (1) {
        if (current_user_id == -1) {
            printf("\n--- Banking Management System ---\n");
            printf("Select Role to Login:\n");
            printf("1. Customer\n2. Bank Employee\n3. Manager\n4. Administrator\n5. Exit\n");
            role_choice = get_int_input("Enter choice: ");

            if (role_choice == 5) break;
            if (role_choice < 1 || role_choice > 4) {
                printf("Invalid role choice.\n");
                continue;
            }

            if (role_choice == 1) {
                printf("\n--- Customer Access ---\n");
                printf("1. Existing Account Login\n");
                printf("2. Create New Account\n");
                int customer_choice = get_int_input("Enter choice: ");

                if (customer_choice == 2) {
                    // --- New Customer Creation Logic ---
                    printf("\n--- New Account Registration ---\n");
                    printf("Enter desired Username (max 49 chars): "); scanf("%s", username);
                    printf("Enter Password: "); scanf("%s", password);
                    clear_input_buffer();

                    snprintf(request, BUFFER_SIZE, "CREATE_CUSTOMER|%d|%s|%s", role_choice, username, password);
                    send_request(request, response);

                    token = strtok(response, "|");
                    if (token != NULL && strcmp(token, "SUCCESS") == 0) {
                        token = strtok(NULL, "|"); // user_id
                        current_user_id = atoi(token);
                        current_role_id = role_choice; 
                        printf("Account created and logged in as User ID: %d.\n", current_user_id);
                        continue; 
                    } else {
                        token = strtok(NULL, "|");
                        printf("Account Creation Failed: %s\n", token ? token : "Unknown server error.");
                        continue;
                    }

                } else if (customer_choice != 1) {
                    printf("Invalid choice. Returning to role selection.\n");
                    continue;
                }
            }
            
            printf("Enter Username: "); scanf("%s", username);
            printf("Enter Password: "); scanf("%s", password);
            clear_input_buffer();

            snprintf(request, BUFFER_SIZE, "LOGIN|%d|%s|%s", role_choice, username, password);
            send_request(request, response);

            token = strtok(response, "|");
            if (token != NULL && strcmp(token, "SUCCESS") == 0) {
                token = strtok(NULL, "|"); // user_id
                current_user_id = atoi(token);
                current_role_id = role_choice; 

                printf("Login successful as User ID: %d (Role %d)\n", current_user_id, current_role_id);
            } else {
                token = strtok(NULL, "|");
                printf("Login Failed: %s\n", token ? token : "Unknown server error.");
            }
        }

        if (current_user_id != -1) {
            switch (current_role_id) {
                case 1: customer_menu(); break;
                case 2: employee_menu(); break;
                case 3: manager_menu(); break;
                case 4: administrator_menu(); break;
            }
        }
    }
    return 0;
}
