#include "MyProject/Account.hpp"

Account::Account(std::string owner, double balance) 
    : m_owner(std::move(owner)), m_balance(balance) {}

void Account::deposit(double amount) {
    if (amount > 0) m_balance += amount;
}

bool Account::withdraw(double amount) {
    if (amount > 0 && m_balance >= amount) {
        m_balance -= amount;
        return true;
    }
    return false;
}

double Account::getBalance() const {
    return m_balance;
}