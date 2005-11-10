/* $Id: router.hpp,v 1.6 2005-11-10 23:10:42 adam Exp $
   Copyright (c) 2005, Index Data.

%LICENSE%
 */

#ifndef ROUTER_HPP
#define ROUTER_HPP

#include <boost/noncopyable.hpp>
#include <string>
#include <stdexcept>

namespace yp2 
{
    namespace filter {
        class Base;
    }
    class Package;
    
    class RouterException : public std::runtime_error {
    public:
        RouterException(const std::string message)
            : std::runtime_error("RouterException: " + message){};
    };
  
    
    class Router : boost::noncopyable {
    public:
        Router(){};
        virtual ~Router(){};

        /// determines next Filter to use from current Filter and Package
        virtual const filter::Base *move(const filter::Base *filter,
                                         const Package *package) const = 0;
        
        /// re-read configuration of routing tables
        //virtual void configure(){};

        /// add routing rule expressed as Filter to Router
        //virtual Router & rule(const filter::Base &filter){
        //    return *this;
        //}
    };
}
#endif
/*
 * Local variables:
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * c-file-style: "stroustrup"
 * End:
 * vim: shiftwidth=4 tabstop=8 expandtab
 */
