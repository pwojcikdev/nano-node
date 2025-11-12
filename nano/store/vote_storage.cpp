#include <nano/store/vote_storage.hpp>
#include <nano/store/typed_iterator_templ.hpp>

template class nano::store::typed_iterator<nano::vote_storage_key, std::shared_ptr<nano::vote>>;
