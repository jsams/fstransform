/*
 * fstransform - transform a file-system to another file-system type,
 *               preserving its contents and without the need for a backup
 *
 * Copyright (C) 2011-2012 Massimiliano Ghilardi
 *
 *     This program is free software: you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation, either version 3 of the License, or
 *     (at your option) any later version.
 *
 *     This program is distributed in the hope that it will be useful,
 *     but WITHOUT ANY WARRANTY; without even the implied warranty of
 *     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *     GNU General Public License for more details.
 *
 *     You should have received a copy of the GNU General Public License
 *     along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * cache_zmem.hh
 *
 *  Created on: Jan 30, 2017
 *      Author: max
 */

#ifndef FSTRANSFORM_CACHE_ZMEM_HH
#define FSTRANSFORM_CACHE_ZMEM_HH

#include "cache.hh"      // for ft_cache
#include "zpaged_map.hh" // for zpaged_map


FT_NAMESPACE_BEGIN

/**
 * compressed in-memory associative array from keys (type K) to values (type V).
 * Used to implement inode cache - see cache.hh for details.
 */
template<class K, class V>
class ft_cache_zmem : public ft_cache<K, V>
{
private:
    typedef ft_cache<K,V> super_type;

    zpaged_map<K,V> map;
    
public:
    /** default constructor */
    ft_cache_zmem(const V & init_zero_payload = V()) : super_type(init_zero_payload), map()
    { }
    
    /** copy constructor */
    ft_cache_zmem(const ft_cache_zmem<K,V> & other) : super_type(other), map(other.map)
    { }
    
    /** assignment operator */
    virtual const super_type & operator=(const ft_cache_zmem<K,V> & other)
    {
        if (this != &other)
            map = other.map;
        return super_type::operator=(other);
    }
    
    /** destructor */
    virtual ~ft_cache_zmem()
    { }
    
    /**
     * if cached inode found, set payload and return 1.
     * Otherwise add it to cache and return 0.
     * On error, return < 0.
     * if returns 0, erase() must be called on the same inode when done with payload!
     */
    virtual int find_or_add(const K key, V & inout_payload)
    {
        ff_assert(inout_payload != this->zero_payload);

        V & value = map[key];
        if (value == this->zero_payload) {
            value = inout_payload;
            return 0;
        }
        inout_payload = value;
        return 1;
    }

    /**
     * if cached key found, set result_payload, remove cached key and return 1.
     * Otherwise return 0. On error, return < 0.
     */
    virtual int find_and_delete(const K key, V & result_payload)
    {
        typename map_type::iterator iter = map.find(key);
        if (iter == map.end())
            return 0;

        result_payload = iter->second;
        map.erase(iter);
        return 1;
    }

    /**
     * if cached inode found, change its payload and return 1.
     * Otherwise return 0. On error, return < 0.
     */
    virtual int find_and_update(const K key, const V & new_payload)
    {
        typename map_type::iterator iter = map.find(key);
        if (iter == map.end())
            return 0;
        
        iter->second = new_payload;
        return 1;
    }
    
    virtual void clear()
    {
        map.clear();
    }
};

FT_NAMESPACE_END

#endif /* FSTRANSFORM_CACHE_MEM_HH */
