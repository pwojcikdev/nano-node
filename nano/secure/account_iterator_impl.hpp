#include <nano/lib/utility.hpp>
#include <nano/secure/account_info.hpp>
#include <nano/secure/account_iterator.hpp>

template <typename Set>
nano::account_iterator<Set>::account_iterator () :
	transaction{ nullptr }
{
}

template <typename Set>
nano::account_iterator<Set>::account_iterator (secure::transaction const & transaction, Set const & set, std::optional<std::pair<nano::account, nano::account_info>> const & item) :
	transaction{ &transaction },
	set{ &set },
	item{ item }
{
}

template <typename Set>
bool nano::account_iterator<Set>::operator== (account_iterator const & other) const
{
	debug_assert (set == nullptr || other.set == nullptr || set == other.set);
	return item == other.item;
}

template <typename Set>
auto nano::account_iterator<Set>::operator++ () -> account_iterator<Set> &
{
	*this = set->account_upper_bound (*transaction, item.value ().first);
	return *this;
}

template <typename Set>
std::pair<nano::account, nano::account_info> const & nano::account_iterator<Set>::operator* () const
{
	return item.value ();
}

template <typename Set>
std::pair<nano::account, nano::account_info> const * nano::account_iterator<Set>::operator->() const
{
	return &item.value ();
}
