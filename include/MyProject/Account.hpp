#pragma once
#include <string>

class Account {
public:
    Account(std::string owner, double balance);
    
    void deposit(double amount);
    bool withdraw(double amount);
    double getBalance() const;

private:
    std::string m_owner;
    double m_balance;
};